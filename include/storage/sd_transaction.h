#pragma once

#include "storage/fram_wal.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace storage {

template <typename Device>
class SdTransactionJournal {
public:
    explicit SdTransactionJournal(Device& device) : device_(device) {}

    FramWalStatus begin(uint32_t lastConsumedSequence) {
        FramSdTransaction a {};
        FramSdTransaction b {};
        if (!device_.read(ADDR_SD_TX_A, reinterpret_cast<uint8_t*>(&a), sizeof(a)) ||
            !device_.read(ADDR_SD_TX_B, reinterpret_cast<uint8_t*>(&b), sizeof(b))) {
            return FramWalStatus::IoError;
        }
        const bool validA = valid(a);
        const bool validB = valid(b);
        if (!validA && !validB) {
            if (a.committed == 1 || b.committed == 1) return FramWalStatus::Corrupt;
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

        // WAL consume committed before transaction cleanup. Complete cleanup
        // now rather than treating it as a conflicting new transaction.
        if (current_.active && current_.sequence == lastConsumedSequence) {
            return finish(current_.sequence);
        }
        return FramWalStatus::Ready;
    }

    bool active() const { return initialized_ && current_.active == 1; }
    const FramSdTransaction& current() const { return current_; }

    FramWalStatus start(uint32_t sequence, const char* filename,
                        const uint8_t* line, uint16_t lineLength) {
        if (!initialized_ || sequence == 0 || filename == nullptr || line == nullptr ||
            filename[0] != '/' || lineLength == 0) {
            return FramWalStatus::Corrupt;
        }
        const size_t filenameLength = strlen(filename);
        if (filenameLength >= sizeof(current_.filename)) return FramWalStatus::Corrupt;
        const uint16_t lineCrc = crc16(line, lineLength);
        if (active()) {
            return current_.sequence == sequence && current_.lineLength == lineLength &&
                   current_.lineCrc16 == lineCrc && strcmp(current_.filename, filename) == 0
                ? FramWalStatus::Ready : FramWalStatus::Corrupt;
        }

        FramSdTransaction next {};
        next.magic = FRAM_SD_TRANSACTION_MAGIC;
        next.transactionVersion = FRAM_SD_TRANSACTION_VERSION;
        next.sequence = sequence;
        next.lineLength = lineLength;
        next.lineCrc16 = lineCrc;
        memcpy(next.filename, filename, filenameLength + 1);
        next.active = 1;
        return persist(next);
    }

    FramWalStatus finish(uint32_t sequence) {
        if (!initialized_) return FramWalStatus::IoError;
        if (!active()) return FramWalStatus::Ready;
        if (current_.sequence != sequence) return FramWalStatus::Corrupt;
        FramSdTransaction next = current_;
        next.active = 0;
        return persist(next);
    }

private:
    Device& device_;
    FramSdTransaction current_ {};
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

    static bool erased(const uint8_t* data, size_t length) {
        bool zero = true;
        bool ones = true;
        for (size_t i = 0; i < length; ++i) {
            zero = zero && data[i] == 0;
            ones = ones && data[i] == 0xFF;
        }
        return zero || ones;
    }

    static bool newer(uint32_t a, uint32_t b) {
        return static_cast<int32_t>(a - b) > 0;
    }

    static bool valid(const FramSdTransaction& tx) {
        return tx.committed == 1 && tx.magic == FRAM_SD_TRANSACTION_MAGIC &&
               tx.transactionVersion == FRAM_SD_TRANSACTION_VERSION &&
               tx.generation != 0 && tx.active <= 1 &&
               tx.filename[sizeof(tx.filename) - 1] == '\0' &&
               crc16(reinterpret_cast<const uint8_t*>(&tx),
                     offsetof(FramSdTransaction, crc16)) == tx.crc16;
    }

    FramWalStatus persist(FramSdTransaction next) {
        next.generation = generation_ + 1;
        if (next.generation == 0) next.generation = 1;
        next.committed = 0;
        next.crc16 = crc16(reinterpret_cast<const uint8_t*>(&next),
                           offsetof(FramSdTransaction, crc16));
        const uint16_t addr = (next.generation & 1u) ? ADDR_SD_TX_A : ADDR_SD_TX_B;
        const uint16_t markerAddr = addr + offsetof(FramSdTransaction, committed);
        if (!device_.writeByte(markerAddr, 0) ||
            !device_.write(addr, reinterpret_cast<const uint8_t*>(&next), sizeof(next)) ||
            !device_.verify(addr, reinterpret_cast<const uint8_t*>(&next), sizeof(next)) ||
            !device_.writeByte(markerAddr, 1)) {
            return FramWalStatus::IoError;
        }
        FramSdTransaction verified {};
        if (!device_.read(addr, reinterpret_cast<uint8_t*>(&verified), sizeof(verified)) ||
            !valid(verified)) {
            return FramWalStatus::IoError;
        }
        current_ = verified;
        generation_ = verified.generation;
        return FramWalStatus::Ready;
    }
};

} // namespace storage
