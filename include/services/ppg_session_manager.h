#pragma once

#include "core/ppg_sample_sink.h"
#include "core/sensor_snapshot.h"
#include "storage/ppg_binary_v1.h"
#include "storage/storage_manager.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>

namespace services {

enum class PpgSessionError : uint8_t {
    None = 0,
    ClockInvalid,
    SdUnavailable,
    FramUnavailable,
    BufferUnavailable,
    PathCollision,
    CreateFailed,
    WriteFailed,
    VerifyFailed,
    RenameFailed,
    NoSamples,
    RecoveryFailed
};

const char* ppgSessionErrorName(PpgSessionError error);

struct PpgSessionStatus {
    storage::PpgJournalState state {storage::PpgJournalState::Empty};
    PpgSessionError error {PpgSessionError::None};
    char sessionId[17] {};
    char directory[64] {};
    uint32_t blockCount {};
    uint32_t storedSamples {};
    uint32_t droppedSamples {};
    uint32_t fifoOverflows {};
    uint32_t ringHighWaterSamples {};
    uint32_t maxSdWriteUs {};
    bool usingPsram {false};
};

class PpgSessionManager : public core::IPpgSampleSink {
public:
    explicit PpgSessionManager(storage::StorageManager& storageManager);
    ~PpgSessionManager();

    bool begin();
    void setDesiredRecording(bool active) { desiredRecording_ = active; }
    void update(const core::SensorSnapshot& snapshot, uint32_t nowMs);
    void onPpgSample(const core::PpgRawSample& sample) override;
    PpgSessionStatus status() const;

private:
    struct RingEntry {
        uint32_t red;
        uint32_t ir;
        uint32_t logicalIndex;
    };

    struct FileScan {
        storage::ppg1::Error error {storage::ppg1::Error::None};
        storage::ppg1::FileHeader header {};
        storage::ppg1::Footer footer {};
        size_t lastValidOffset {};
        uint32_t blockCount {};
        uint32_t sampleCount {};
        uint32_t nextLogicalSampleIndex {};
        uint32_t streamCrc32 {};
        bool complete {false};
    };

    static constexpr size_t PSRAM_RING_CAPACITY = 4096;
    static constexpr size_t INTERNAL_RING_CAPACITY = 1024;
    static constexpr size_t BLOCK_SAMPLE_CAPACITY = 512;
    static constexpr size_t BLOCK_PAYLOAD_SIZE =
        BLOCK_SAMPLE_CAPACITY * storage::ppg1::SAMPLE_SIZE;

    storage::StorageManager& storage_;
    RingEntry* ring_ {nullptr};
    size_t ringCapacity_ {};
    size_t ringHead_ {};
    size_t ringTail_ {};
    size_t ringCount_ {};
    uint32_t ringDrops_ {};
    uint32_t ringHighWater_ {};
    bool sourceBaseSet_ {false};
    uint32_t sourceBaseIndex_ {};
    mutable portMUX_TYPE ringMux_ = portMUX_INITIALIZER_UNLOCKED;
    mutable portMUX_TYPE statusMux_ = portMUX_INITIALIZER_UNLOCKED;
    PpgSessionStatus publishedStatus_ {};

    std::atomic<bool> desiredRecording_ {false};
    std::atomic<bool> capturing_ {false};
    std::atomic<uint8_t> state_ {
        static_cast<uint8_t>(storage::PpgJournalState::Empty)};
    bool recoveryNeeded_ {false};
    uint32_t nextStartAttemptMs_ {};
    uint32_t lastEnvironmentMs_ {};
    uint32_t lastDropEventCount_ {};
    uint32_t lastDropEventMs_ {};

    char sessionId_[17] {};
    char directory_[64] {};
    char rawTmpPath_[96] {};
    char rawPath_[96] {};
    char environmentTmpPath_[96] {};
    char environmentPath_[96] {};
    char metadataTmpPath_[96] {};
    char metadataPath_[96] {};

    uint64_t startUnixUs_ {};
    uint32_t startMonotonicMs_ {};
    uint32_t blockCount_ {};
    uint32_t storedSamples_ {};
    uint32_t startDriverDrops_ {};
    uint32_t startFifoOverflows_ {};
    uint32_t latestDriverDrops_ {};
    uint32_t latestFifoOverflows_ {};
    uint16_t redLedCurrentX10Ma_ {};
    uint16_t irLedCurrentX10Ma_ {};
    uint32_t maxSdWriteUs_ {};
    uint32_t environmentRows_ {};
    bool startEnvironmentRecorded_ {false};
    bool endEnvironmentRecorded_ {false};
    bool usingPsram_ {false};
    PpgSessionError lastError_ {PpgSessionError::None};
    storage::ppg1::Crc32 streamCrc_;
    uint8_t blockPayload_[BLOCK_PAYLOAD_SIZE] {};

    void setState(storage::PpgJournalState state);
    void publishStatus(uint32_t droppedSamples = UINT32_MAX,
                       uint32_t fifoOverflows = UINT32_MAX);
    void resetRing();
    bool popBlock(uint32_t& firstSampleIndex, uint16_t& sampleCount,
                  bool force);
    bool startSession(const core::SensorSnapshot& snapshot, uint32_t nowMs);
    bool appendBlock(uint32_t firstSampleIndex, uint16_t sampleCount,
                     uint32_t nowMs);
    bool finalizeSession(const core::SensorSnapshot& snapshot,
                         uint32_t nowMs, bool recovered,
                         bool recoveredPartial);
    void failSession(PpgSessionError error, uint32_t nowMs);
    bool persistCheckpoint(storage::PpgJournalState state);
    bool createSessionFiles(const core::SensorSnapshot& snapshot);
    bool appendEnvironment(const core::SensorSnapshot& snapshot,
                           const char* phase, uint32_t nowMs);
    bool writeMetadata(const core::SensorSnapshot& snapshot,
                       const char* completionStatus,
                       const char* reason,
                       uint64_t endUnixUs,
                       uint32_t rawSize,
                       uint32_t finalStreamCrc,
                       uint32_t droppedOverride = UINT32_MAX,
                       uint32_t fifoOverflowOverride = UINT32_MAX);
    bool verifyAndRename(bool recoveredPartial);
    bool recoverPending(const core::SensorSnapshot& snapshot,
                        uint32_t nowMs);
    FileScan scanRawFile(const char* path);
    bool copyValidRawPrefix(const char* sourcePath, const char* recoveryPath,
                            const FileScan& scan, uint64_t endUnixUs,
                            uint32_t droppedSamples,
                            uint32_t fifoOverflows);
    bool recoverEnvironmentFile();
    bool buildPaths(uint64_t startUnixUs);
    static bool formatIsoUtc(uint64_t unixUs, char* output, size_t capacity);
    static uint32_t elapsed(uint32_t nowMs, uint32_t thenMs) {
        return nowMs - thenMs;
    }
};

} // namespace services
