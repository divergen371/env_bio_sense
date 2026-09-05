#include <unity.h>
#include "storage/ppg_session_journal.h"
#include <cstring>

namespace {

class FakeFram {
public:
    FakeFram() { std::memset(bytes_, 0xFF, sizeof(bytes_)); }
    uint32_t getCapacity() const { return sizeof(bytes_); }
    bool read(uint16_t address, uint8_t* output, size_t length) {
        if (fail_ || address + length > sizeof(bytes_)) return false;
        std::memcpy(output, bytes_ + address, length);
        return true;
    }
    bool write(uint16_t address, const uint8_t* input, size_t length) {
        if (fail_ || address + length > sizeof(bytes_)) return false;
        std::memcpy(bytes_ + address, input, length);
        return true;
    }
    bool writeByte(uint16_t address, uint8_t value) {
        return write(address, &value, 1);
    }
    bool verify(uint16_t address, const uint8_t* expected, size_t length) {
        return !fail_ && address + length <= sizeof(bytes_) &&
            std::memcmp(bytes_ + address, expected, length) == 0;
    }
    void corrupt(uint16_t address) { bytes_[address] ^= 0x55; }
    void fail(bool value) { fail_ = value; }

private:
    uint8_t bytes_[storage::FRAM_CAPACITY];
    bool fail_ {false};
};

storage::FramPpgCheckpoint recordingCheckpoint() {
    storage::FramPpgCheckpoint checkpoint {};
    checkpoint.startUnixUs = UINT64_C(1788652800123456);
    checkpoint.blockCount = 4;
    checkpoint.sampleCount = 2048;
    checkpoint.fileSize = 16560;
    checkpoint.streamCrc32 = 0x12345678;
    checkpoint.droppedSamples = 3;
    checkpoint.fifoOverflows = 1;
    checkpoint.state = static_cast<uint8_t>(
        storage::PpgJournalState::Recording);
    return checkpoint;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_empty_journal_initializes_without_writing(void) {
    FakeFram fram;
    storage::PpgSessionJournal<FakeFram> journal(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(journal.begin()));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::PpgJournalState::Empty),
        journal.current().state);
}

void test_checkpoint_survives_reopen(void) {
    FakeFram fram;
    storage::PpgSessionJournal<FakeFram> writer(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(writer.begin()));
    storage::FramPpgCheckpoint checkpoint = recordingCheckpoint();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(writer.persist(checkpoint)));

    storage::PpgSessionJournal<FakeFram> reader(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(reader.begin()));
    TEST_ASSERT_EQUAL_UINT64(checkpoint.startUnixUs,
                             reader.current().startUnixUs);
    TEST_ASSERT_EQUAL_UINT32(2048, reader.current().sampleCount);
}

void test_newest_valid_copy_wins(void) {
    FakeFram fram;
    storage::PpgSessionJournal<FakeFram> journal(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(journal.begin()));
    storage::FramPpgCheckpoint first = recordingCheckpoint();
    storage::FramPpgCheckpoint second = first;
    second.sampleCount = 4096;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(journal.persist(first)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(journal.persist(second)));
    storage::PpgSessionJournal<FakeFram> reopened(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(reopened.begin()));
    TEST_ASSERT_EQUAL_UINT32(4096, reopened.current().sampleCount);
}

void test_torn_new_copy_falls_back_to_previous(void) {
    FakeFram fram;
    storage::PpgSessionJournal<FakeFram> journal(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(journal.begin()));
    storage::FramPpgCheckpoint checkpoint = recordingCheckpoint();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(journal.persist(checkpoint)));
    const uint16_t unusedCopy = storage::ADDR_PPG_CHECKPOINT_B;
    uint8_t partial[8] {1, 2, 3, 4, 5, 6, 7, 8};
    TEST_ASSERT_TRUE(fram.write(unusedCopy, partial, sizeof(partial)));

    storage::PpgSessionJournal<FakeFram> reopened(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(reopened.begin()));
    TEST_ASSERT_EQUAL_UINT32(2048, reopened.current().sampleCount);
}

void test_committed_corrupt_only_copy_is_rejected(void) {
    FakeFram fram;
    storage::PpgSessionJournal<FakeFram> journal(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(journal.begin()));
    storage::FramPpgCheckpoint checkpoint = recordingCheckpoint();
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Ready),
        static_cast<uint8_t>(journal.persist(checkpoint)));
    fram.corrupt(storage::ADDR_PPG_CHECKPOINT_A +
                 offsetof(storage::FramPpgCheckpoint, sampleCount));
    storage::PpgSessionJournal<FakeFram> reopened(fram);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::FramWalStatus::Corrupt),
        static_cast<uint8_t>(reopened.begin()));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_journal_initializes_without_writing);
    RUN_TEST(test_checkpoint_survives_reopen);
    RUN_TEST(test_newest_valid_copy_wins);
    RUN_TEST(test_torn_new_copy_falls_back_to_previous);
    RUN_TEST(test_committed_corrupt_only_copy_is_rejected);
    return UNITY_END();
}
