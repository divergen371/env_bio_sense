#pragma once

#include "storage/storage_records.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace storage {

enum class FramWalStatus : uint8_t {
    Ready,
    Full,
    Empty,
    Corrupt,
    Incompatible,
    IoError,
    SequenceExhausted
};

// Hardware-independent WAL used by StorageManager and native fault-injection
// tests. Device must expose read(), write(), writeByte(), verify(), and
// getCapacity().
template <typename Device>
class FramWal {
public:
    explicit FramWal(Device& device) : device_(device) {}

    FramWalStatus begin(FramSuperblock& state, FramWalStats& stats,
                        const FramSuperblock& initialState) {
        if (device_.getCapacity() < FRAM_CAPACITY) return FramWalStatus::Incompatible;
        FramCheckpoint a {};
        FramCheckpoint b {};
        const bool readA = device_.read(ADDR_CHECKPOINT_A,
            reinterpret_cast<uint8_t*>(&a), sizeof(a));
        const bool readB = device_.read(ADDR_CHECKPOINT_B,
            reinterpret_cast<uint8_t*>(&b), sizeof(b));
        if (!readA || !readB) return FramWalStatus::IoError;

        const bool validA = validCheckpoint(a);
        const bool validB = validCheckpoint(b);
        if (validA || validB) {
            const FramCheckpoint& chosen = validA && validB
                ? (isNewer(a.generation, b.generation) ? a : b)
                : (validA ? a : b);
            if (!validState(chosen.superblock)) return FramWalStatus::Corrupt;
            state = chosen.superblock;
            stats = chosen.stats;
            generation_ = chosen.generation;
            initialized_ = true;
            return reconcileOrphan(state, stats);
        }

        FramSuperblock legacy {};
        if (!device_.read(ADDR_SUPERBLOCK,
                reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy))) {
            return FramWalStatus::IoError;
        }
        if (legacy.magic == FRAM_MAGIC) {
            if (legacy.formatVersion != FRAM_FORMAT_VERSION &&
                legacy.formatVersion != LEGACY_FRAM_FORMAT_VERSION) {
                return FramWalStatus::Incompatible;
            }
            if (!validLegacy(legacy) || !validState(legacy)) {
                return FramWalStatus::Corrupt;
            }
            state = legacy;
            stats = {};
            stats.highWaterRecords = pendingCount(state);
        } else {
            // A committed-but-invalid checkpoint is evidence of corruption.
            // An uncommitted partial checkpoint is safe to ignore because no
            // record write is permitted before initialization succeeds.
            if (a.committed == 1 || b.committed == 1) return FramWalStatus::Corrupt;
            if (!deviceBlank()) return FramWalStatus::Corrupt;
            state = initialState;
            stats = {};
        }

        initialized_ = true;
        generation_ = 0;
        FramWalStatus persisted = persist(state, stats);
        return persisted == FramWalStatus::Ready ? reconcileOrphan(state, stats) : persisted;
    }

    FramWalStatus persist(FramSuperblock& state, const FramWalStats& stats) {
        if (!initialized_ || !validState(state)) return FramWalStatus::Corrupt;

        // Preserve a valid inner CRC for recovery tools that understand the
        // original v5 superblock.
        state.crc16 = crc16(reinterpret_cast<const uint8_t*>(&state),
                            sizeof(state) - sizeof(state.crc16));

        FramCheckpoint cp {};
        cp.magic = FRAM_CHECKPOINT_MAGIC;
        cp.checkpointVersion = FRAM_CHECKPOINT_VERSION;
        cp.generation = generation_ + 1;
        if (cp.generation == 0) cp.generation = 1;
        cp.superblock = state;
        cp.stats = stats;
        cp.committed = 0;
        cp.crc16 = crc16(reinterpret_cast<const uint8_t*>(&cp),
                         offsetof(FramCheckpoint, crc16));

        const uint16_t addr = (cp.generation & 1u)
            ? ADDR_CHECKPOINT_A : ADDR_CHECKPOINT_B;
        const uint16_t markerAddr = addr + offsetof(FramCheckpoint, committed);

        // Invalidate the destination first. A torn overwrite must not retain
        // an old committed marker beside new partial bytes.
        if (!device_.writeByte(markerAddr, 0)) return FramWalStatus::IoError;
        if (!device_.write(addr, reinterpret_cast<const uint8_t*>(&cp), sizeof(cp))) {
            return FramWalStatus::IoError;
        }
        if (!device_.verify(addr, reinterpret_cast<const uint8_t*>(&cp), sizeof(cp))) {
            return FramWalStatus::IoError;
        }
        if (!device_.writeByte(markerAddr, 1)) return FramWalStatus::IoError;

        FramCheckpoint verified {};
        if (!device_.read(addr, reinterpret_cast<uint8_t*>(&verified), sizeof(verified)) ||
            !validCheckpoint(verified)) {
            return FramWalStatus::IoError;
        }
        generation_ = verified.generation;
        return FramWalStatus::Ready;
    }

    FramWalStatus append(SensorRecordV6 data, FramSuperblock& state,
                         FramWalStats& stats) {
        data.formatTag = 6;
        return appendTyped<SensorRecordV6, PersistentRecordV6>(
            data, state, stats, FRAM_FORMAT_VERSION);
    }

    FramWalStatus appendLegacy(SensorRecordV5 data, FramSuperblock& state,
                               FramWalStats& stats) {
        return appendTyped<SensorRecordV5, PersistentRecordV5>(
            data, state, stats, LEGACY_FRAM_FORMAT_VERSION);
    }

    FramWalStatus migrateEmptyToCurrent(FramSuperblock& state,
                                        const FramWalStats& stats) {
        if (!initialized_) return FramWalStatus::IoError;
        if (pendingCount(state) != 0) return FramWalStatus::Incompatible;
        if (state.formatVersion == FRAM_FORMAT_VERSION) return FramWalStatus::Ready;
        if (state.formatVersion != LEGACY_FRAM_FORMAT_VERSION) {
            return FramWalStatus::Incompatible;
        }
        FramSuperblock next = state;
        next.formatVersion = FRAM_FORMAT_VERSION;
        const FramWalStatus result = persist(next, stats);
        if (result == FramWalStatus::Ready) state = next;
        return result;
    }

    FramWalStatus peek(const FramSuperblock& state, PersistentRecordV6& record) {
        if (pendingCount(state) == 0) return FramWalStatus::Empty;
        if (state.formatVersion != FRAM_FORMAT_VERSION) {
            return FramWalStatus::Incompatible;
        }
        return readRecordV6(state.readIndex, record);
    }

    FramWalStatus peekLegacy(const FramSuperblock& state,
                             PersistentRecordV5& record) {
        if (pendingCount(state) == 0) return FramWalStatus::Empty;
        if (state.formatVersion != LEGACY_FRAM_FORMAT_VERSION) {
            return FramWalStatus::Incompatible;
        }
        return readRecordV5(state.readIndex, record);
    }

    FramWalStatus readRawSlot(uint16_t index, uint8_t* raw, size_t length) {
        if (index >= MAX_RECORDS || raw == nullptr || length != RECORD_SLOT_SIZE) {
            return FramWalStatus::Corrupt;
        }
        const uint16_t addr = ADDR_RING_BUFFER + index * RECORD_SLOT_SIZE;
        return device_.read(addr, raw, length)
            ? FramWalStatus::Ready : FramWalStatus::IoError;
    }

    // Advances past a record only after the caller has durably quarantined its
    // raw bytes elsewhere. Unlike consume(), this deliberately leaves
    // lastSdFlushSequence unchanged because no CSV row was committed.
    FramWalStatus quarantineTail(uint16_t expectedIndex, FramSuperblock& state,
                                 const FramWalStats& stats) {
        if (!initialized_) return FramWalStatus::IoError;
        if (pendingCount(state) == 0) return FramWalStatus::Empty;
        if (expectedIndex != state.readIndex) return FramWalStatus::Corrupt;

        FramSuperblock next = state;
        next.readIndex = (next.readIndex + 1) % MAX_RECORDS;
        const FramWalStatus result = persist(next, stats);
        if (result == FramWalStatus::Ready) state = next;
        return result;
    }

    FramWalStatus consume(uint32_t sequence, FramSuperblock& state,
                          const FramWalStats& stats) {
        FramWalStatus result = FramWalStatus::Incompatible;
        uint32_t storedSequence = 0;
        if (state.formatVersion == FRAM_FORMAT_VERSION) {
            PersistentRecordV6 record {};
            result = peek(state, record);
            storedSequence = record.header.sequence;
        } else if (state.formatVersion == LEGACY_FRAM_FORMAT_VERSION) {
            PersistentRecordV5 record {};
            result = peekLegacy(state, record);
            storedSequence = record.header.sequence;
        }
        if (result != FramWalStatus::Ready) return result;
        if (storedSequence != sequence) return FramWalStatus::Corrupt;

        FramSuperblock next = state;
        next.readIndex = (next.readIndex + 1) % MAX_RECORDS;
        next.lastSdFlushSequence = sequence;
        result = persist(next, stats);
        if (result == FramWalStatus::Ready) state = next;
        return result;
    }

    static uint16_t pendingCount(const FramSuperblock& state) {
        return state.writeIndex >= state.readIndex
            ? state.writeIndex - state.readIndex
            : MAX_RECORDS - state.readIndex + state.writeIndex;
    }

private:
    template <typename Data, typename Persistent>
    FramWalStatus appendTyped(Data data, FramSuperblock& state,
                              FramWalStats& stats,
                              uint16_t expectedFormatVersion) {
        if (!initialized_) return FramWalStatus::IoError;
        if (state.formatVersion != expectedFormatVersion) {
            return FramWalStatus::Incompatible;
        }
        const uint16_t pending = pendingCount(state);
        if (pending >= MAX_RECORDS - 1) {
            if (stats.droppedRecords != UINT32_MAX) ++stats.droppedRecords;
            FramWalStatus result = persist(state, stats);
            return result == FramWalStatus::Ready ? FramWalStatus::Full : result;
        }
        if (state.nextSequence == UINT32_MAX) return FramWalStatus::SequenceExhausted;

        Persistent rec {};
        data.sequence = state.nextSequence;
        rec.data = data;
        rec.header.sequence = data.sequence;
        rec.header.length = sizeof(Data);
        rec.header.crc = crc16(reinterpret_cast<const uint8_t*>(&rec.data),
                               sizeof(Data));
        rec.header.committed = 0;

        const uint16_t addr = ADDR_RING_BUFFER + state.writeIndex * RECORD_SLOT_SIZE;
        const uint16_t markerAddr = addr + offsetof(FramRecordHeader, committed);
        if (!device_.writeByte(markerAddr, 0) ||
            !device_.write(addr, reinterpret_cast<const uint8_t*>(&rec), sizeof(rec)) ||
            !device_.verify(addr, reinterpret_cast<const uint8_t*>(&rec), sizeof(rec)) ||
            !device_.writeByte(markerAddr, 1)) {
            return FramWalStatus::IoError;
        }

        Persistent verified {};
        if (!device_.read(addr, reinterpret_cast<uint8_t*>(&verified),
                          sizeof(verified))) {
            return FramWalStatus::IoError;
        }
        const bool formatTagValid =
            expectedFormatVersion != FRAM_FORMAT_VERSION ||
            reinterpret_cast<const uint8_t*>(&verified.data)[sizeof(Data) - 1] == 6;
        if (verified.header.committed != 1 ||
            verified.header.length != sizeof(Data) ||
            verified.header.sequence != state.nextSequence ||
            verified.header.sequence != verified.data.sequence ||
            !formatTagValid ||
            crc16(reinterpret_cast<const uint8_t*>(&verified.data),
                  sizeof(Data)) != verified.header.crc) {
            return FramWalStatus::IoError;
        }

        FramSuperblock next = state;
        FramWalStats nextStats = stats;
        next.writeIndex = (next.writeIndex + 1) % MAX_RECORDS;
        ++next.nextSequence;
        const uint16_t nextPending = pendingCount(next);
        if (nextPending > nextStats.highWaterRecords) {
            nextStats.highWaterRecords = nextPending;
        }
        FramWalStatus result = persist(next, nextStats);
        if (result == FramWalStatus::Ready) {
            state = next;
            stats = nextStats;
        }
        return result;
    }
    Device& device_;
    uint32_t generation_ {0};
    bool initialized_ {false};

    static uint16_t crc16(const uint8_t* data, size_t length) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (uint8_t bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) ? (crc >> 1) ^ 0xA001 : crc >> 1;
            }
        }
        return crc;
    }

    static bool isNewer(uint32_t a, uint32_t b) {
        return static_cast<int32_t>(a - b) > 0;
    }

    static bool erased(const uint8_t* data, size_t length) {
        bool allZero = true;
        bool allOnes = true;
        for (size_t i = 0; i < length; ++i) {
            allZero = allZero && data[i] == 0x00;
            allOnes = allOnes && data[i] == 0xFF;
        }
        return allZero || allOnes;
    }

    static bool validState(const FramSuperblock& state) {
        return state.magic == FRAM_MAGIC &&
               (state.formatVersion == FRAM_FORMAT_VERSION ||
                state.formatVersion == LEGACY_FRAM_FORMAT_VERSION) &&
               state.writeIndex < MAX_RECORDS && state.readIndex < MAX_RECORDS &&
               state.nextSequence != 0;
    }

    static bool validLegacy(const FramSuperblock& state) {
        return crc16(reinterpret_cast<const uint8_t*>(&state),
                     sizeof(state) - sizeof(state.crc16)) == state.crc16;
    }

    static bool validCheckpoint(const FramCheckpoint& cp) {
        return cp.committed == 1 && cp.magic == FRAM_CHECKPOINT_MAGIC &&
               cp.checkpointVersion == FRAM_CHECKPOINT_VERSION &&
               cp.generation != 0 &&
               crc16(reinterpret_cast<const uint8_t*>(&cp),
                     offsetof(FramCheckpoint, crc16)) == cp.crc16;
    }

    bool deviceBlank() {
        if (device_.getCapacity() < FRAM_CAPACITY) return false;
        uint8_t buffer[32];
        bool allZero = true;
        bool allOnes = true;
        for (uint32_t addr = 0; addr < FRAM_CAPACITY; addr += sizeof(buffer)) {
            if ((addr >= ADDR_CHECKPOINT_A &&
                 addr < ADDR_CHECKPOINT_A + CHECKPOINT_SLOT_SIZE) ||
                (addr >= ADDR_CHECKPOINT_B &&
                 addr < ADDR_CHECKPOINT_B + CHECKPOINT_SLOT_SIZE)) {
                continue;
            }
            if (!device_.read(static_cast<uint16_t>(addr), buffer, sizeof(buffer))) return false;
            for (uint8_t byte : buffer) {
                allZero = allZero && byte == 0x00;
                allOnes = allOnes && byte == 0xFF;
            }
        }
        return allZero || allOnes;
    }

    FramWalStatus readRecordV5(uint16_t index, PersistentRecordV5& record) {
        if (index >= MAX_RECORDS) return FramWalStatus::Corrupt;
        const uint16_t addr = ADDR_RING_BUFFER + index * RECORD_SLOT_SIZE;
        if (!device_.read(addr, reinterpret_cast<uint8_t*>(&record), sizeof(record))) {
            return FramWalStatus::IoError;
        }
        const bool supportedLength = record.header.length == sizeof(SensorRecordV5) ||
            record.header.length == LEGACY_SENSOR_RECORD_V5_SIZE;
        if (record.header.committed != 1 || !supportedLength ||
            record.header.sequence == 0 || record.header.sequence != record.data.sequence) {
            return FramWalStatus::Corrupt;
        }
        return crc16(reinterpret_cast<const uint8_t*>(&record.data), record.header.length) ==
               record.header.crc ? FramWalStatus::Ready : FramWalStatus::Corrupt;
    }

    FramWalStatus readRecordV6(uint16_t index, PersistentRecordV6& record) {
        if (index >= MAX_RECORDS) return FramWalStatus::Corrupt;
        const uint16_t addr = ADDR_RING_BUFFER + index * RECORD_SLOT_SIZE;
        if (!device_.read(addr, reinterpret_cast<uint8_t*>(&record), sizeof(record))) {
            return FramWalStatus::IoError;
        }
        if (record.header.committed != 1 ||
            record.header.length != sizeof(SensorRecordV6) ||
            record.header.sequence == 0 ||
            record.header.sequence != record.data.sequence ||
            record.data.formatTag != 6) {
            return FramWalStatus::Corrupt;
        }
        return crc16(reinterpret_cast<const uint8_t*>(&record.data),
                     record.header.length) == record.header.crc
            ? FramWalStatus::Ready : FramWalStatus::Corrupt;
    }

    FramWalStatus reconcileOrphan(FramSuperblock& state, FramWalStats& stats) {
        if (pendingCount(state) >= MAX_RECORDS - 1 || state.nextSequence == UINT32_MAX) {
            return FramWalStatus::Ready;
        }
        FramWalStatus orphanStatus = FramWalStatus::Incompatible;
        uint32_t orphanSequence = 0;
        if (state.formatVersion == FRAM_FORMAT_VERSION) {
            PersistentRecordV6 orphan {};
            orphanStatus = readRecordV6(state.writeIndex, orphan);
            orphanSequence = orphan.header.sequence;
        } else if (state.formatVersion == LEGACY_FRAM_FORMAT_VERSION) {
            PersistentRecordV5 orphan {};
            orphanStatus = readRecordV5(state.writeIndex, orphan);
            orphanSequence = orphan.header.sequence;
        }
        if (orphanStatus == FramWalStatus::IoError) return orphanStatus;
        if (orphanStatus != FramWalStatus::Ready ||
            orphanSequence != state.nextSequence) {
            return FramWalStatus::Ready;
        }

        FramSuperblock next = state;
        FramWalStats nextStats = stats;
        next.writeIndex = (next.writeIndex + 1) % MAX_RECORDS;
        ++next.nextSequence;
        const uint16_t nextPending = pendingCount(next);
        if (nextPending > nextStats.highWaterRecords) nextStats.highWaterRecords = nextPending;
        FramWalStatus result = persist(next, nextStats);
        if (result == FramWalStatus::Ready) {
            state = next;
            stats = nextStats;
        }
        return result;
    }
};

} // namespace storage
