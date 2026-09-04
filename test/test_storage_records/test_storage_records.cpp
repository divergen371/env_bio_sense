#include <unity.h>
#include "core/sensor_types.h"
#include "storage/storage_records.h"
#include "storage/fram_wal.h"
#include "storage/sd_transaction.h"
#include <array>
#include <cstring>

using namespace storage;

namespace {

class FakeFram {
public:
    std::array<uint8_t, FRAM_CAPACITY> bytes;
    int failWriteCall = -1;
    int writeCalls = 0;
    bool failVerify = false;
    bool failRead = false;

    FakeFram() { bytes.fill(0xFF); }

    uint32_t getCapacity() const { return bytes.size(); }
    bool read(uint16_t address, uint8_t* out, size_t length) {
        if (failRead || static_cast<size_t>(address) + length > bytes.size()) return false;
        memcpy(out, bytes.data() + address, length);
        return true;
    }
    bool write(uint16_t address, const uint8_t* data, size_t length) {
        ++writeCalls;
        if (static_cast<size_t>(address) + length > bytes.size()) return false;
        if (writeCalls == failWriteCall) {
            memcpy(bytes.data() + address, data, length / 2);
            return false;
        }
        memcpy(bytes.data() + address, data, length);
        return true;
    }
    bool writeByte(uint16_t address, uint8_t value) { return write(address, &value, 1); }
    bool verify(uint16_t address, const uint8_t* data, size_t length) {
        return !failVerify && static_cast<size_t>(address) + length <= bytes.size() &&
               memcmp(bytes.data() + address, data, length) == 0;
    }
};

uint16_t testCrc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

FramSuperblock initialState() {
    FramSuperblock state {};
    state.magic = FRAM_MAGIC;
    state.formatVersion = FRAM_FORMAT_VERSION;
    state.nextSequence = 1;
    return state;
}

} // namespace

void test_v5_quality_extension_fits_existing_slot() {
    TEST_ASSERT_EQUAL_UINT16(117, LEGACY_SENSOR_RECORD_V5_SIZE);
    TEST_ASSERT_EQUAL_UINT16(119, sizeof(SensorRecordV5));
    TEST_ASSERT_EQUAL_UINT16(128, sizeof(PersistentRecordV5));
}

void test_scd41_state_uses_only_reserved_flag_bits() {
    uint32_t flags = VALID_TEMP | VALID_CO2;
    uint32_t encoded = flags |
        (static_cast<uint32_t>(core::DeviceState::RetryWait) << SCD41_STATE_SHIFT);

    TEST_ASSERT_EQUAL_UINT32(flags, encoded & ~SCD41_STATE_MASK);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::DeviceState::RetryWait),
        static_cast<uint8_t>((encoded & SCD41_STATE_MASK) >> SCD41_STATE_SHIFT));
}

void test_event_journal_geometry_and_payload() {
    TEST_ASSERT_EQUAL_UINT16(19, sizeof(EventRecord));
    TEST_ASSERT_EQUAL_UINT16(10, EVENT_PAYLOAD_SIZE);
    TEST_ASSERT_EQUAL_UINT16(112, MAX_EVENT_RECORDS);
    TEST_ASSERT_EQUAL_STRING("SCD41_RECOVERED", eventCodeName(EventCode::Scd41Recovered));
}

void test_wal_fresh_device_initializes_without_erasing_ring() {
    FakeFram fram;
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));
    TEST_ASSERT_EQUAL_UINT32(1, state.nextSequence);
    TEST_ASSERT_EQUAL_UINT16(0, FramWal<FakeFram>::pendingCount(state));
    for (size_t i = ADDR_RING_BUFFER; i < fram.bytes.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, fram.bytes[i]);
    }
}

void test_wal_append_readback_and_consume() {
    FakeFram fram;
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));

    SensorRecordV6 sample {};
    sample.co2Ppm = 777;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.append(sample, state, stats)));
    TEST_ASSERT_EQUAL_UINT16(1, FramWal<FakeFram>::pendingCount(state));

    PersistentRecordV6 stored {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.peek(state, stored)));
    TEST_ASSERT_EQUAL_UINT32(1, stored.header.sequence);
    TEST_ASSERT_EQUAL_UINT16(777, stored.data.co2Ppm);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.consume(1, state, stats)));
    TEST_ASSERT_EQUAL_UINT16(0, FramWal<FakeFram>::pendingCount(state));
    TEST_ASSERT_EQUAL_UINT32(1, state.lastSdFlushSequence);
}

void test_wal_recovers_every_append_write_boundary() {
    for (int failedWrite = 1; failedWrite <= 6; ++failedWrite) {
        FakeFram fram;
        FramSuperblock state {};
        FramWalStats stats {};
        {
            FramWal<FakeFram> wal(fram);
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
                static_cast<uint8_t>(wal.begin(state, stats, initialState())));
            fram.writeCalls = 0;
            fram.failWriteCall = failedWrite;
            SensorRecordV6 sample {};
            sample.co2Ppm = 500 + failedWrite;
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::IoError),
                static_cast<uint8_t>(wal.append(sample, state, stats)));
        }

        fram.failWriteCall = -1;
        fram.writeCalls = 0;
        FramWal<FakeFram> recovered(fram);
        FramSuperblock recoveredState {};
        FramWalStats recoveredStats {};
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
            static_cast<uint8_t>(recovered.begin(
                recoveredState, recoveredStats, initialState())));
        const uint16_t pending = FramWal<FakeFram>::pendingCount(recoveredState);
        TEST_ASSERT_TRUE(pending == 0 || pending == 1);
        if (pending == 1) {
            PersistentRecordV6 record {};
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
                static_cast<uint8_t>(recovered.peek(recoveredState, record)));
            TEST_ASSERT_EQUAL_UINT32(1, record.header.sequence);
        } else {
            TEST_ASSERT_EQUAL_UINT32(1, recoveredState.nextSequence);
        }
    }
}

void test_wal_uses_older_checkpoint_when_newest_is_corrupt() {
    FakeFram fram;
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));
    state.bootCount = 42;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.persist(state, stats)));
    fram.bytes[ADDR_CHECKPOINT_B + offsetof(FramCheckpoint, superblock.bootCount)] ^= 0x40;

    FramWal<FakeFram> recovered(fram);
    FramSuperblock recoveredState {};
    FramWalStats recoveredStats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.begin(recoveredState, recoveredStats, initialState())));
    TEST_ASSERT_EQUAL_UINT32(0, recoveredState.bootCount);
}

void test_wal_refuses_unknown_legacy_format() {
    FakeFram fram;
    FramSuperblock legacy = initialState();
    legacy.formatVersion = 99;
    legacy.crc16 = testCrc16(reinterpret_cast<const uint8_t*>(&legacy),
                             sizeof(legacy) - sizeof(legacy.crc16));
    memcpy(fram.bytes.data() + ADDR_SUPERBLOCK, &legacy, sizeof(legacy));
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Incompatible),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));
}

void test_v5_pending_records_are_preserved_until_consumed_then_migrated() {
    FakeFram fram;
    FramSuperblock legacy = initialState();
    legacy.formatVersion = LEGACY_FRAM_FORMAT_VERSION;
    legacy.crc16 = testCrc16(reinterpret_cast<const uint8_t*>(&legacy),
                             sizeof(legacy) - sizeof(legacy.crc16));
    memcpy(fram.bytes.data() + ADDR_SUPERBLOCK, &legacy, sizeof(legacy));

    FramWal<FakeFram> legacyWal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(legacyWal.begin(state, stats, initialState())));
    TEST_ASSERT_EQUAL_UINT16(LEGACY_FRAM_FORMAT_VERSION, state.formatVersion);

    SensorRecordV5 oldSample {};
    oldSample.co2Ppm = 812;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(legacyWal.appendLegacy(oldSample, state, stats)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Incompatible),
        static_cast<uint8_t>(legacyWal.migrateEmptyToCurrent(state, stats)));

    std::array<uint8_t, RECORD_SLOT_SIZE> oldSlot {};
    memcpy(oldSlot.data(), fram.bytes.data() + ADDR_RING_BUFFER,
           oldSlot.size());

    FramWal<FakeFram> recovered(fram);
    FramSuperblock recoveredState {};
    FramWalStats recoveredStats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.begin(
            recoveredState, recoveredStats, initialState())));
    PersistentRecordV5 recoveredOld {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.peekLegacy(recoveredState, recoveredOld)));
    TEST_ASSERT_EQUAL_UINT16(812, recoveredOld.data.co2Ppm);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.consume(
            recoveredOld.header.sequence, recoveredState, recoveredStats)));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.migrateEmptyToCurrent(
            recoveredState, recoveredStats)));
    TEST_ASSERT_EQUAL_UINT16(FRAM_FORMAT_VERSION, recoveredState.formatVersion);
    TEST_ASSERT_EQUAL_MEMORY(oldSlot.data(),
        fram.bytes.data() + ADDR_RING_BUFFER, oldSlot.size());

    SensorRecordV6 newSample {};
    newSample.co2Ppm = 913;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.append(
            newSample, recoveredState, recoveredStats)));
    PersistentRecordV6 recoveredNew {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.peek(recoveredState, recoveredNew)));
    TEST_ASSERT_EQUAL_UINT16(913, recoveredNew.data.co2Ppm);
    TEST_ASSERT_EQUAL_UINT8(6, recoveredNew.data.formatTag);
}

void test_wal_refuses_to_initialize_corrupt_nonblank_fram() {
    FakeFram fram;
    fram.bytes[ADDR_RING_BUFFER + 10] = 0x12;
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Corrupt),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));
}

void test_wal_full_preserves_tail_and_persists_drop_count() {
    FakeFram fram;
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));
    state.writeIndex = MAX_RECORDS - 1;
    state.readIndex = 0;
    state.nextSequence = MAX_RECORDS;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.persist(state, stats)));

    SensorRecordV6 sample {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Full),
        static_cast<uint8_t>(wal.append(sample, state, stats)));
    TEST_ASSERT_EQUAL_UINT16(0, state.readIndex);
    TEST_ASSERT_EQUAL_UINT16(MAX_RECORDS - 1, state.writeIndex);
    TEST_ASSERT_EQUAL_UINT32(1, stats.droppedRecords);

    FramWal<FakeFram> recovered(fram);
    FramSuperblock recoveredState {};
    FramWalStats recoveredStats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.begin(
            recoveredState, recoveredStats, initialState())));
    TEST_ASSERT_EQUAL_UINT32(1, recoveredStats.droppedRecords);
    TEST_ASSERT_EQUAL_UINT16(0, recoveredState.readIndex);
}

void test_wal_quarantine_advances_only_tail_without_claiming_sd_flush() {
    FakeFram fram;
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));

    SensorRecordV6 sample {};
    sample.co2Ppm = 500;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.append(sample, state, stats)));
    sample.co2Ppm = 600;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.append(sample, state, stats)));

    fram.bytes[ADDR_RING_BUFFER + offsetof(PersistentRecordV6, header.crc)] ^= 0x40;
    PersistentRecordV6 record {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Corrupt),
        static_cast<uint8_t>(wal.peek(state, record)));
    uint8_t raw[RECORD_SLOT_SIZE] {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.readRawSlot(state.readIndex, raw, sizeof(raw))));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.quarantineTail(0, state, stats)));
    TEST_ASSERT_EQUAL_UINT16(1, FramWal<FakeFram>::pendingCount(state));
    TEST_ASSERT_EQUAL_UINT32(0, state.lastSdFlushSequence);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.peek(state, record)));
    TEST_ASSERT_EQUAL_UINT32(2, record.header.sequence);
}

void test_wal_quarantine_refuses_wrong_or_empty_tail() {
    FakeFram fram;
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Empty),
        static_cast<uint8_t>(wal.quarantineTail(0, state, stats)));
    SensorRecordV6 sample {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.append(sample, state, stats)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Corrupt),
        static_cast<uint8_t>(wal.quarantineTail(1, state, stats)));
    TEST_ASSERT_EQUAL_UINT16(0, state.readIndex);
}

void test_wal_peek_distinguishes_io_error_from_corruption() {
    FakeFram fram;
    FramWal<FakeFram> wal(fram);
    FramSuperblock state {};
    FramWalStats stats {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.begin(state, stats, initialState())));
    SensorRecordV6 sample {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(wal.append(sample, state, stats)));
    fram.failRead = true;
    PersistentRecordV6 record {};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::IoError),
        static_cast<uint8_t>(wal.peek(state, record)));
}

void test_wal_quarantine_checkpoint_failure_never_skips_without_commit() {
    for (int failedWrite = 1; failedWrite <= 3; ++failedWrite) {
        FakeFram fram;
        FramSuperblock state {};
        FramWalStats stats {};
        {
            FramWal<FakeFram> wal(fram);
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
                static_cast<uint8_t>(wal.begin(state, stats, initialState())));
            SensorRecordV6 sample {};
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
                static_cast<uint8_t>(wal.append(sample, state, stats)));
            fram.bytes[ADDR_RING_BUFFER + offsetof(PersistentRecordV6, header.crc)] ^= 0x20;
            fram.writeCalls = 0;
            fram.failWriteCall = failedWrite;
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::IoError),
                static_cast<uint8_t>(wal.quarantineTail(0, state, stats)));
            TEST_ASSERT_EQUAL_UINT16(0, state.readIndex);
            TEST_ASSERT_EQUAL_UINT32(0, state.lastSdFlushSequence);
        }

        fram.failWriteCall = -1;
        fram.writeCalls = 0;
        FramWal<FakeFram> recovered(fram);
        FramSuperblock recoveredState {};
        FramWalStats recoveredStats {};
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
            static_cast<uint8_t>(recovered.begin(
                recoveredState, recoveredStats, initialState())));
        TEST_ASSERT_EQUAL_UINT16(0, recoveredState.readIndex);
        TEST_ASSERT_EQUAL_UINT16(1, FramWal<FakeFram>::pendingCount(recoveredState));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
            static_cast<uint8_t>(recovered.quarantineTail(
                0, recoveredState, recoveredStats)));
        TEST_ASSERT_EQUAL_UINT16(0, FramWal<FakeFram>::pendingCount(recoveredState));
    }
}

void test_sd_transaction_survives_reset_and_preserves_target() {
    FakeFram fram;
    SdTransactionJournal<FakeFram> tx(fram);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(tx.begin(0)));
    const char line[] = "17,5000,example";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(tx.start(17, "/log_boot_003_v6.csv",
            reinterpret_cast<const uint8_t*>(line), sizeof(line) - 1)));

    SdTransactionJournal<FakeFram> recovered(fram);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.begin(0)));
    TEST_ASSERT_TRUE(recovered.active());
    TEST_ASSERT_EQUAL_UINT32(17, recovered.current().sequence);
    TEST_ASSERT_EQUAL_STRING("/log_boot_003_v6.csv", recovered.current().filename);
}

void test_sd_transaction_cleans_up_after_wal_consume() {
    FakeFram fram;
    SdTransactionJournal<FakeFram> tx(fram);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(tx.begin(0)));
    const char line[] = "21,10000,example";
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(tx.start(21, "/log_20260903_v6.csv",
            reinterpret_cast<const uint8_t*>(line), sizeof(line) - 1)));

    SdTransactionJournal<FakeFram> recovered(fram);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
        static_cast<uint8_t>(recovered.begin(21)));
    TEST_ASSERT_FALSE(recovered.active());
}

void test_sd_transaction_torn_start_is_safe_to_retry() {
    for (int failedWrite = 1; failedWrite <= 3; ++failedWrite) {
        FakeFram fram;
        {
            SdTransactionJournal<FakeFram> tx(fram);
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
                static_cast<uint8_t>(tx.begin(0)));
            fram.failWriteCall = failedWrite;
            fram.writeCalls = 0;
            const char line[] = "1,5000";
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::IoError),
                static_cast<uint8_t>(tx.start(1, "/log_boot_000_v6.csv",
                    reinterpret_cast<const uint8_t*>(line), sizeof(line) - 1)));
        }
        fram.failWriteCall = -1;
        fram.writeCalls = 0;
        SdTransactionJournal<FakeFram> recovered(fram);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FramWalStatus::Ready),
            static_cast<uint8_t>(recovered.begin(0)));
        TEST_ASSERT_FALSE(recovered.active());
    }
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_v5_quality_extension_fits_existing_slot);
    RUN_TEST(test_scd41_state_uses_only_reserved_flag_bits);
    RUN_TEST(test_event_journal_geometry_and_payload);
    RUN_TEST(test_wal_fresh_device_initializes_without_erasing_ring);
    RUN_TEST(test_wal_append_readback_and_consume);
    RUN_TEST(test_wal_recovers_every_append_write_boundary);
    RUN_TEST(test_wal_uses_older_checkpoint_when_newest_is_corrupt);
    RUN_TEST(test_wal_refuses_unknown_legacy_format);
    RUN_TEST(test_v5_pending_records_are_preserved_until_consumed_then_migrated);
    RUN_TEST(test_wal_refuses_to_initialize_corrupt_nonblank_fram);
    RUN_TEST(test_wal_full_preserves_tail_and_persists_drop_count);
    RUN_TEST(test_wal_quarantine_advances_only_tail_without_claiming_sd_flush);
    RUN_TEST(test_wal_quarantine_refuses_wrong_or_empty_tail);
    RUN_TEST(test_wal_peek_distinguishes_io_error_from_corruption);
    RUN_TEST(test_wal_quarantine_checkpoint_failure_never_skips_without_commit);
    RUN_TEST(test_sd_transaction_survives_reset_and_preserves_target);
    RUN_TEST(test_sd_transaction_cleans_up_after_wal_consume);
    RUN_TEST(test_sd_transaction_torn_start_is_safe_to_retry);
    return UNITY_END();
}
