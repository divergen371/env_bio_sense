#pragma once

#include "storage/fram_wal.h"
#include "storage/storage_records.h"
#include <cstddef>
#include <cstdint>

namespace storage {

template <typename Device>
class PpgSessionJournal {
public:
    explicit PpgSessionJournal(Device& device) : device_(device) {}

    FramWalStatus begin() {
        FramPpgCheckpoint a {};
        FramPpgCheckpoint b {};
        if (!device_.read(ADDR_PPG_CHECKPOINT_A,
                reinterpret_cast<uint8_t*>(&a), sizeof(a)) ||
            !device_.read(ADDR_PPG_CHECKPOINT_B,
                reinterpret_cast<uint8_t*>(&b), sizeof(b))) {
            return FramWalStatus::IoError;
        }
        const bool validA = valid(a);
        const bool validB = valid(b);
        if (!validA && !validB) {
            if (a.committed == 1 || b.committed == 1) {
                return FramWalStatus::Corrupt;
            }
            current_ = {};
            generation_ = 0;
            initialized_ = true;
            return FramWalStatus::Ready;
        }
        current_ = validA && validB
            ? (newer(a.generation, b.generation) ? a : b)
            : (validA ? a : b);
        generation_ = current_.generation;
        initialized_ = true;
        return FramWalStatus::Ready;
    }

    bool initialized() const { return initialized_; }
    const FramPpgCheckpoint& current() const { return current_; }

    FramWalStatus persist(FramPpgCheckpoint next) {
        if (!initialized_ || !sane(next)) return FramWalStatus::Corrupt;
        next.magic = FRAM_PPG_CHECKPOINT_MAGIC;
        next.checkpointVersion = FRAM_PPG_CHECKPOINT_VERSION;
        next.generation = generation_ + 1;
        if (next.generation == 0) next.generation = 1;
        next.committed = 0;
        next.crc16 = crc16(reinterpret_cast<const uint8_t*>(&next),
                           offsetof(FramPpgCheckpoint, crc16));
        const uint16_t address = (next.generation & 1u)
            ? ADDR_PPG_CHECKPOINT_A : ADDR_PPG_CHECKPOINT_B;
        const uint16_t markerAddress = address +
            offsetof(FramPpgCheckpoint, committed);
        if (!device_.writeByte(markerAddress, 0) ||
            !device_.write(address, reinterpret_cast<const uint8_t*>(&next),
                           sizeof(next)) ||
            !device_.verify(address,
                reinterpret_cast<const uint8_t*>(&next), sizeof(next)) ||
            !device_.writeByte(markerAddress, 1)) {
            return FramWalStatus::IoError;
        }
        FramPpgCheckpoint verified {};
        if (!device_.read(address, reinterpret_cast<uint8_t*>(&verified),
                          sizeof(verified)) || !valid(verified)) {
            return FramWalStatus::IoError;
        }
        current_ = verified;
        generation_ = verified.generation;
        return FramWalStatus::Ready;
    }

    FramWalStatus clear() {
        FramPpgCheckpoint empty {};
        empty.state = static_cast<uint8_t>(PpgJournalState::Empty);
        return persist(empty);
    }

private:
    Device& device_;
    FramPpgCheckpoint current_ {};
    uint32_t generation_ {};
    bool initialized_ {false};

    static uint16_t crc16(const uint8_t* data, size_t length) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (uint8_t bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) ? (crc >> 1u) ^ 0xA001u : crc >> 1u;
            }
        }
        return crc;
    }

    static bool newer(uint32_t left, uint32_t right) {
        return static_cast<int32_t>(left - right) > 0;
    }

    static bool sane(const FramPpgCheckpoint& checkpoint) {
        if (checkpoint.state >
            static_cast<uint8_t>(PpgJournalState::Failed)) return false;
        if (checkpoint.state ==
            static_cast<uint8_t>(PpgJournalState::Empty)) return true;
        return checkpoint.startUnixUs != 0;
    }

    static bool valid(const FramPpgCheckpoint& checkpoint) {
        return checkpoint.committed == 1 &&
            checkpoint.magic == FRAM_PPG_CHECKPOINT_MAGIC &&
            checkpoint.checkpointVersion ==
                FRAM_PPG_CHECKPOINT_VERSION &&
            checkpoint.generation != 0 && sane(checkpoint) &&
            crc16(reinterpret_cast<const uint8_t*>(&checkpoint),
                  offsetof(FramPpgCheckpoint, crc16)) == checkpoint.crc16;
    }
};

} // namespace storage
