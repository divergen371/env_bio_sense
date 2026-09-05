#include "services/ppg_session_manager.h"
#include "hal/clock.h"
#include "services/logger.h"
#include "utils/ppg_timing.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace services {
namespace {

constexpr const char* ENV_HEADER =
    "record_id,phase,offset_ms,timestamp,temp_c,rh_pct,pressure_pa,co2_ppm,voc_index,nox_index,status_bits";

bool appendText(char* output, size_t capacity, size_t& length,
                const char* format, ...) {
    if (output == nullptr || format == nullptr || length >= capacity) {
        return false;
    }
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(
        output + length, capacity - length, format, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= capacity - length) {
        return false;
    }
    length += static_cast<size_t>(written);
    return true;
}

bool appendOptionalFloat(char* output, size_t capacity, size_t& length,
                         bool valid, float value, unsigned decimals) {
    if (!valid || !std::isfinite(value)) {
        return appendText(output, capacity, length, ",");
    }
    char format[12] {};
    std::snprintf(format, sizeof(format), "%%.%uf,", decimals);
    return appendText(output, capacity, length, format, value);
}

bool readExact(File& file, uint8_t* output, size_t length) {
    return output != nullptr && file.read(output, length) == length;
}

bool writeExact(File& file, const uint8_t* input, size_t length) {
    return input != nullptr && file.write(input, length) == length;
}

uint32_t nonNegativeDelta(uint32_t current, uint32_t start) {
    return current >= start ? current - start : current;
}

} // namespace

const char* ppgSessionErrorName(PpgSessionError error) {
    switch (error) {
        case PpgSessionError::None: return "NONE";
        case PpgSessionError::ClockInvalid: return "CLOCK_INVALID";
        case PpgSessionError::SdUnavailable: return "SD_UNAVAILABLE";
        case PpgSessionError::FramUnavailable: return "FRAM_UNAVAILABLE";
        case PpgSessionError::BufferUnavailable: return "BUFFER_UNAVAILABLE";
        case PpgSessionError::PathCollision: return "PATH_COLLISION";
        case PpgSessionError::CreateFailed: return "CREATE_FAILED";
        case PpgSessionError::WriteFailed: return "WRITE_FAILED";
        case PpgSessionError::VerifyFailed: return "VERIFY_FAILED";
        case PpgSessionError::RenameFailed: return "RENAME_FAILED";
        case PpgSessionError::NoSamples: return "NO_SAMPLES";
        case PpgSessionError::RecoveryFailed: return "RECOVERY_FAILED";
    }
    return "UNKNOWN";
}

PpgSessionManager::PpgSessionManager(
        storage::StorageManager& storageManager)
    : storage_(storageManager) {}

PpgSessionManager::~PpgSessionManager() {
    if (ring_ != nullptr) {
        heap_caps_free(ring_);
        ring_ = nullptr;
    }
}

bool PpgSessionManager::begin() {
    ring_ = static_cast<RingEntry*>(heap_caps_malloc(
        sizeof(RingEntry) * PSRAM_RING_CAPACITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ring_ != nullptr) {
        ringCapacity_ = PSRAM_RING_CAPACITY;
        usingPsram_ = true;
    } else {
        ring_ = static_cast<RingEntry*>(heap_caps_malloc(
            sizeof(RingEntry) * INTERNAL_RING_CAPACITY,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        ringCapacity_ = ring_ == nullptr ? 0 : INTERNAL_RING_CAPACITY;
        usingPsram_ = false;
    }
    if (ring_ == nullptr) {
        lastError_ = PpgSessionError::BufferUnavailable;
        publishStatus();
        Logger::error("PpgSession", "Unable to allocate PPG ring buffer");
        return false;
    }
    Logger::info("PpgSession",
        "PPG ring ready: %u samples, %.1f seconds, memory=%s",
        static_cast<unsigned>(ringCapacity_),
        ringCapacity_ /
            static_cast<float>(utils::ppg_timing::EFFECTIVE_SAMPLE_RATE_HZ),
        usingPsram_ ? "PSRAM" : "INTERNAL");

    storage::FramPpgCheckpoint checkpoint {};
    if (!storage_.getPpgCheckpoint(checkpoint)) {
        lastError_ = PpgSessionError::FramUnavailable;
        publishStatus();
        Logger::error("PpgSession",
            "PPG checkpoint journal unavailable; recording disabled");
        return false;
    }
    const storage::PpgJournalState saved =
        static_cast<storage::PpgJournalState>(checkpoint.state);
    if (saved == storage::PpgJournalState::Preparing ||
        saved == storage::PpgJournalState::Recording ||
        saved == storage::PpgJournalState::Finalizing ||
        saved == storage::PpgJournalState::RecoveryPending) {
        startUnixUs_ = checkpoint.startUnixUs;
        blockCount_ = checkpoint.blockCount;
        storedSamples_ = checkpoint.sampleCount;
        recoveryNeeded_ = true;
        setState(storage::PpgJournalState::RecoveryPending);
        Logger::warn("PpgSession",
            "Interrupted PPG session found; recovery scheduled");
    } else {
        if (saved != storage::PpgJournalState::Empty) {
            storage_.clearPpgCheckpoint();
        }
        setState(storage::PpgJournalState::Empty);
    }
    publishStatus();
    return true;
}

void PpgSessionManager::setState(storage::PpgJournalState state) {
    state_ = static_cast<uint8_t>(state);
}

void PpgSessionManager::publishStatus(uint32_t droppedSamples,
                                      uint32_t fifoOverflows) {
    portENTER_CRITICAL(&statusMux_);
    const PpgSessionStatus previous = publishedStatus_;
    portEXIT_CRITICAL(&statusMux_);
    PpgSessionStatus next;
    next.state = static_cast<storage::PpgJournalState>(state_.load());
    next.error = lastError_;
    std::memcpy(next.sessionId, sessionId_, sizeof(sessionId_));
    std::memcpy(next.directory, directory_, sizeof(directory_));
    next.blockCount = blockCount_;
    next.storedSamples = storedSamples_;
    next.usingPsram = usingPsram_;
    next.maxSdWriteUs = maxSdWriteUs_;
    portENTER_CRITICAL(&ringMux_);
    next.ringHighWaterSamples = ringHighWater_;
    if (droppedSamples == UINT32_MAX) {
        next.droppedSamples = previous.droppedSamples;
    } else {
        next.droppedSamples = droppedSamples;
    }
    if (fifoOverflows == UINT32_MAX) {
        next.fifoOverflows = previous.fifoOverflows;
    } else {
        next.fifoOverflows = fifoOverflows;
    }
    portEXIT_CRITICAL(&ringMux_);
    portENTER_CRITICAL(&statusMux_);
    publishedStatus_ = next;
    portEXIT_CRITICAL(&statusMux_);
}

PpgSessionStatus PpgSessionManager::status() const {
    portENTER_CRITICAL(&statusMux_);
    const PpgSessionStatus copy = publishedStatus_;
    portEXIT_CRITICAL(&statusMux_);
    return copy;
}

void PpgSessionManager::resetRing() {
    portENTER_CRITICAL(&ringMux_);
    ringHead_ = 0;
    ringTail_ = 0;
    ringCount_ = 0;
    ringDrops_ = 0;
    ringHighWater_ = 0;
    sourceBaseSet_ = false;
    sourceBaseIndex_ = 0;
    portEXIT_CRITICAL(&ringMux_);
}

void PpgSessionManager::onPpgSample(const core::PpgRawSample& sample) {
    if (!capturing_.load() || ring_ == nullptr || ringCapacity_ == 0) return;
    portENTER_CRITICAL(&ringMux_);
    if (!sourceBaseSet_) {
        sourceBaseIndex_ = sample.logicalIndex;
        sourceBaseSet_ = true;
    }
    if (ringCount_ >= ringCapacity_) {
        ++ringDrops_;
        portEXIT_CRITICAL(&ringMux_);
        return;
    }
    RingEntry& entry = ring_[ringHead_];
    entry.red = sample.red & storage::ppg1::MAX30102_VALUE_MASK;
    entry.ir = sample.ir & storage::ppg1::MAX30102_VALUE_MASK;
    entry.logicalIndex = sample.logicalIndex - sourceBaseIndex_;
    ringHead_ = (ringHead_ + 1) % ringCapacity_;
    ++ringCount_;
    ringHighWater_ = std::max<uint32_t>(
        ringHighWater_, static_cast<uint32_t>(ringCount_));
    portEXIT_CRITICAL(&ringMux_);
}

bool PpgSessionManager::popBlock(uint32_t& firstSampleIndex,
                                 uint16_t& sampleCount, bool force) {
    sampleCount = 0;
    portENTER_CRITICAL(&ringMux_);
    if (ringCount_ == 0 || (!force && ringCount_ < BLOCK_SAMPLE_CAPACITY)) {
        portEXIT_CRITICAL(&ringMux_);
        return false;
    }
    const uint32_t first = ring_[ringTail_].logicalIndex;
    uint32_t expected = first;
    while (ringCount_ > 0 && sampleCount < BLOCK_SAMPLE_CAPACITY) {
        const RingEntry& entry = ring_[ringTail_];
        if (entry.logicalIndex != expected) break;
        storage::ppg1::encodeSample(
            entry.red, entry.ir,
            blockPayload_ + static_cast<size_t>(sampleCount) *
                storage::ppg1::SAMPLE_SIZE);
        ringTail_ = (ringTail_ + 1) % ringCapacity_;
        --ringCount_;
        ++sampleCount;
        ++expected;
    }
    firstSampleIndex = first;
    portEXIT_CRITICAL(&ringMux_);
    return sampleCount != 0;
}

bool PpgSessionManager::formatIsoUtc(uint64_t unixUs, char* output,
                                     size_t capacity) {
    if (unixUs == 0 || output == nullptr || capacity < 25) return false;
    const time_t epoch = static_cast<time_t>(unixUs / UINT64_C(1000000));
    const unsigned milliseconds =
        static_cast<unsigned>((unixUs / 1000u) % 1000u);
    struct tm value {};
    if (gmtime_r(&epoch, &value) == nullptr) return false;
    const int written = std::snprintf(output, capacity,
        "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
        value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
        value.tm_hour, value.tm_min, value.tm_sec, milliseconds);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool PpgSessionManager::buildPaths(uint64_t startUnixUs) {
    const time_t epoch = static_cast<time_t>(
        startUnixUs / UINT64_C(1000000));
    struct tm value {};
    if (startUnixUs == 0 || gmtime_r(&epoch, &value) == nullptr) return false;
    if (std::snprintf(sessionId_, sizeof(sessionId_),
            "%04d%02d%02dT%02d%02d%02dZ",
            value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
            value.tm_hour, value.tm_min, value.tm_sec) != 16) return false;
    if (std::snprintf(directory_, sizeof(directory_),
            "/data/ppg/%04d-%02d-%02d/%02d%02d%02d",
            value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
            value.tm_hour, value.tm_min, value.tm_sec) <= 0) return false;
    const struct PathTarget { char* output; size_t capacity; const char* name; }
        targets[] = {
            {rawTmpPath_, sizeof(rawTmpPath_), "raw.ppg.tmp"},
            {rawPath_, sizeof(rawPath_), "raw.ppg"},
            {environmentTmpPath_, sizeof(environmentTmpPath_),
             "environment.csv.tmp"},
            {environmentPath_, sizeof(environmentPath_), "environment.csv"},
            {metadataTmpPath_, sizeof(metadataTmpPath_), "metadata.json.tmp"},
            {metadataPath_, sizeof(metadataPath_), "metadata.json"}
        };
    for (const PathTarget& target : targets) {
        const int written = std::snprintf(target.output, target.capacity,
            "%s/%s", directory_, target.name);
        if (written <= 0 || static_cast<size_t>(written) >= target.capacity) {
            return false;
        }
    }
    return true;
}

bool PpgSessionManager::persistCheckpoint(
        storage::PpgJournalState state) {
    storage::FramPpgCheckpoint checkpoint {};
    checkpoint.startUnixUs = startUnixUs_;
    checkpoint.blockCount = blockCount_;
    checkpoint.sampleCount = storedSamples_;
    checkpoint.fileSize = static_cast<uint32_t>(
        storage::ppg1::FILE_HEADER_SIZE +
        blockCount_ * storage::ppg1::BLOCK_HEADER_SIZE +
        storedSamples_ * storage::ppg1::SAMPLE_SIZE);
    checkpoint.streamCrc32 = streamCrc_.value();
    portENTER_CRITICAL(&ringMux_);
    checkpoint.droppedSamples = ringDrops_ + nonNegativeDelta(
        latestDriverDrops_, startDriverDrops_);
    portEXIT_CRITICAL(&ringMux_);
    checkpoint.fifoOverflows = nonNegativeDelta(
        latestFifoOverflows_, startFifoOverflows_);
    checkpoint.state = static_cast<uint8_t>(state);
    if (!storage_.savePpgCheckpoint(checkpoint)) {
        lastError_ = PpgSessionError::FramUnavailable;
        return false;
    }
    setState(state);
    return true;
}

bool PpgSessionManager::appendEnvironment(
        const core::SensorSnapshot& snapshot, const char* phase,
        uint32_t nowMs) {
    const core::TimeSnapshot clock = hal::Clock::snapshot();
    char timestamp[32] {};
    if (!clock.utcValid || !formatIsoUtc(
            static_cast<uint64_t>(clock.utcEpochUs), timestamp,
            sizeof(timestamp))) return false;
    uint32_t statusBits = 0;
    if (snapshot.environment.temperatureValid) statusBits |= 1u << 0;
    if (snapshot.environment.humidityValid) statusBits |= 1u << 1;
    if (snapshot.environment.pressureValid) statusBits |= 1u << 2;
    if (snapshot.environment.co2Valid) statusBits |= 1u << 3;
    if (snapshot.environment.sgp41Valid) statusBits |= 1u << 4;

    char line[384] {};
    size_t length = 0;
    bool ok = appendText(line, sizeof(line), length,
        "%s-%06lu,%s,%lu,%s,", sessionId_,
        static_cast<unsigned long>(environmentRows_ + 1), phase,
        static_cast<unsigned long>(elapsed(nowMs, startMonotonicMs_)),
        timestamp);
    ok = ok && appendOptionalFloat(line, sizeof(line), length,
        snapshot.environment.temperatureValid,
        snapshot.environment.temperatureC, 2);
    ok = ok && appendOptionalFloat(line, sizeof(line), length,
        snapshot.environment.humidityValid,
        snapshot.environment.humidityRh, 2);
    ok = ok && appendOptionalFloat(line, sizeof(line), length,
        snapshot.environment.pressureValid,
        snapshot.environment.pressureHpa * 100.0f, 0);
    if (snapshot.environment.co2Valid) {
        ok = ok && appendText(line, sizeof(line), length, "%u,",
            snapshot.environment.co2Ppm);
    } else {
        ok = ok && appendText(line, sizeof(line), length, ",");
    }
    if (snapshot.environment.sgp41Valid) {
        ok = ok && appendText(line, sizeof(line), length, "%ld,%ld,",
            static_cast<long>(snapshot.environment.vocIndex),
            static_cast<long>(snapshot.environment.noxIndex));
    } else {
        ok = ok && appendText(line, sizeof(line), length, ",,");
    }
    ok = ok && appendText(line, sizeof(line), length, "%lu",
        static_cast<unsigned long>(statusBits));
    if (!ok) return false;

    storage_.lock();
    File before = SD.open(environmentTmpPath_, FILE_READ);
    const uint32_t originalSize = before ? before.size() : 0;
    if (before) before.close();
    File output = SD.open(environmentTmpPath_, FILE_APPEND);
    const bool wrote = output &&
        writeExact(output, reinterpret_cast<const uint8_t*>(line), length) &&
        writeExact(output, reinterpret_cast<const uint8_t*>("\n"), 1);
    if (output) {
        output.flush();
        output.close();
    }
    File verify = wrote ? SD.open(environmentTmpPath_, FILE_READ) : File();
    bool verified = verify && verify.size() == originalSize + length + 1 &&
        verify.seek(originalSize);
    char actual[384] {};
    verified = verified && verify.read(
        reinterpret_cast<uint8_t*>(actual), length + 1) == length + 1 &&
        std::memcmp(actual, line, length) == 0 && actual[length] == '\n';
    if (verify) verify.close();
    storage_.unlock();
    if (verified) ++environmentRows_;
    return verified;
}

bool PpgSessionManager::createSessionFiles(
        const core::SensorSnapshot& snapshot) {
    storage::ppg1::FileHeader header;
    header.startUnixUs = startUnixUs_;
    header.sampleRateHz = utils::ppg_timing::EFFECTIVE_SAMPLE_RATE_HZ;
    header.sampleAverage = utils::ppg_timing::SAMPLE_AVERAGE;
    header.pulseWidthUs = 411;
    header.adcRangeNa = 4096;
    header.redLedCurrentX10Ma = redLedCurrentX10Ma_;
    header.irLedCurrentX10Ma = irLedCurrentX10Ma_;
    header.blockPayloadMax = BLOCK_PAYLOAD_SIZE;
    header.sessionIdHash = storage::ppg1::fnv1a64(sessionId_);
    uint8_t encoded[storage::ppg1::FILE_HEADER_SIZE] {};
    storage::ppg1::encodeFileHeader(header, encoded);

    char dayDirectory[32] {};
    std::memcpy(dayDirectory, directory_, std::min(
        sizeof(dayDirectory) - 1, static_cast<size_t>(20)));
    // /data/ppg/YYYY-MM-DD is always 20 characters.
    dayDirectory[20] = '\0';

    storage_.lock();
    const bool collision = SD.exists(rawTmpPath_) || SD.exists(rawPath_) ||
        SD.exists(environmentTmpPath_) || SD.exists(environmentPath_) ||
        SD.exists(metadataTmpPath_) || SD.exists(metadataPath_);
    bool ok = !collision && (SD.exists("/data") || SD.mkdir("/data")) &&
        (SD.exists("/data/ppg") || SD.mkdir("/data/ppg")) &&
        (SD.exists(dayDirectory) || SD.mkdir(dayDirectory)) &&
        (SD.exists(directory_) || SD.mkdir(directory_));
    if (ok) {
        File raw = SD.open(rawTmpPath_, FILE_WRITE);
        ok = raw && writeExact(raw, encoded, sizeof(encoded));
        if (raw) {
            raw.flush();
            raw.close();
        }
    }
    if (ok) {
        File environment = SD.open(environmentTmpPath_, FILE_WRITE);
        ok = environment && writeExact(environment,
            reinterpret_cast<const uint8_t*>(ENV_HEADER),
            std::strlen(ENV_HEADER)) &&
            writeExact(environment,
                reinterpret_cast<const uint8_t*>("\n"), 1);
        if (environment) {
            environment.flush();
            environment.close();
        }
    }
    if (ok) {
        File metadata = SD.open(metadataTmpPath_, FILE_WRITE);
        ok = static_cast<bool>(metadata);
        if (metadata) metadata.close();
    }
    if (ok) {
        File raw = SD.open(rawTmpPath_, FILE_READ);
        uint8_t actual[storage::ppg1::FILE_HEADER_SIZE] {};
        storage::ppg1::FileHeader decoded;
        ok = raw && raw.size() == sizeof(actual) &&
            readExact(raw, actual, sizeof(actual)) &&
            storage::ppg1::decodeFileHeader(actual, sizeof(actual), decoded) ==
                storage::ppg1::Error::None &&
            decoded.sessionIdHash == header.sessionIdHash;
        if (raw) raw.close();
    }
    storage_.unlock();
    if (!ok) {
        lastError_ = collision ? PpgSessionError::PathCollision
                               : PpgSessionError::CreateFailed;
        return false;
    }
    startEnvironmentRecorded_ = appendEnvironment(
        snapshot, "START", startMonotonicMs_);
    return startEnvironmentRecorded_;
}

bool PpgSessionManager::startSession(
        const core::SensorSnapshot& snapshot, uint32_t nowMs) {
    if (ring_ == nullptr) {
        lastError_ = PpgSessionError::BufferUnavailable;
        return false;
    }
    const core::TimeSnapshot clock = hal::Clock::snapshot();
    if (!clock.utcValid || clock.utcEpochUs <= 0) {
        lastError_ = PpgSessionError::ClockInvalid;
        storage_.appendEvent(storage::EventCode::PpgStartRejected,
            static_cast<int32_t>(lastError_), nowMs);
        nextStartAttemptMs_ = nowMs + 30000u;
        publishStatus();
        return false;
    }
    if (!storage_.isSdAvailable()) {
        lastError_ = PpgSessionError::SdUnavailable;
        storage_.appendEvent(storage::EventCode::PpgStartRejected,
            static_cast<int32_t>(lastError_), nowMs);
        nextStartAttemptMs_ = nowMs + 30000u;
        publishStatus();
        return false;
    }
    if (!storage_.isPpgJournalAvailable()) {
        lastError_ = PpgSessionError::FramUnavailable;
        nextStartAttemptMs_ = nowMs + 30000u;
        publishStatus();
        return false;
    }

    startUnixUs_ = static_cast<uint64_t>(clock.utcEpochUs);
    startMonotonicMs_ = nowMs;
    blockCount_ = 0;
    storedSamples_ = 0;
    startDriverDrops_ = snapshot.ppg.droppedSamples;
    startFifoOverflows_ = snapshot.ppg.fifoOverflows;
    latestDriverDrops_ = snapshot.ppg.droppedSamples;
    latestFifoOverflows_ = snapshot.ppg.fifoOverflows;
    redLedCurrentX10Ma_ = snapshot.ppg.redLedCurrentX10Ma;
    irLedCurrentX10Ma_ = snapshot.ppg.irLedCurrentX10Ma;
    maxSdWriteUs_ = 0;
    environmentRows_ = 0;
    startEnvironmentRecorded_ = false;
    endEnvironmentRecorded_ = false;
    lastDropEventCount_ = 0;
    lastDropEventMs_ = 0;
    lastEnvironmentMs_ = nowMs;
    lastError_ = PpgSessionError::None;
    streamCrc_.reset();
    resetRing();
    if (!buildPaths(startUnixUs_) ||
        !persistCheckpoint(storage::PpgJournalState::Preparing)) {
        failSession(PpgSessionError::FramUnavailable, nowMs);
        return false;
    }
    if (!createSessionFiles(snapshot)) {
        failSession(lastError_, nowMs);
        return false;
    }
    if (!persistCheckpoint(storage::PpgJournalState::Recording)) {
        failSession(PpgSessionError::FramUnavailable, nowMs);
        return false;
    }
    capturing_ = true;
    storage_.appendEvent(storage::EventCode::PpgSessionStarted,
        static_cast<int32_t>(startUnixUs_ / UINT64_C(1000000)), nowMs);
    publishStatus(0, 0);
    Logger::info("PpgSession", "Recording started: %s", sessionId_);
    return true;
}

bool PpgSessionManager::appendBlock(uint32_t firstSampleIndex,
                                    uint16_t sampleCount,
                                    uint32_t nowMs) {
    if (sampleCount == 0 || sampleCount > BLOCK_SAMPLE_CAPACITY) return false;
    const uint32_t payloadSize =
        static_cast<uint32_t>(sampleCount) * storage::ppg1::SAMPLE_SIZE;
    storage::ppg1::BlockHeader block;
    block.blockIndex = blockCount_;
    block.firstSampleIndex = firstSampleIndex;
    block.sampleCount = sampleCount;
    block.payloadSize = payloadSize;
    block.payloadCrc32 = storage::ppg1::crc32(blockPayload_, payloadSize);
    uint8_t header[storage::ppg1::BLOCK_HEADER_SIZE] {};
    storage::ppg1::encodeBlockHeader(block, header);

    const uint32_t startedUs = micros();
    storage_.lock();
    File before = SD.open(rawTmpPath_, FILE_READ);
    const uint32_t originalSize = before ? before.size() : 0;
    if (before) before.close();
    File output = SD.open(rawTmpPath_, FILE_APPEND);
    bool ok = output && writeExact(output, header, sizeof(header)) &&
        writeExact(output, blockPayload_, payloadSize);
    if (output) {
        output.flush();
        output.close();
    }
    File verify = ok ? SD.open(rawTmpPath_, FILE_READ) : File();
    ok = verify && verify.size() ==
        originalSize + sizeof(header) + payloadSize &&
        verify.seek(originalSize);
    uint8_t actualHeader[storage::ppg1::BLOCK_HEADER_SIZE] {};
    ok = ok && readExact(verify, actualHeader, sizeof(actualHeader)) &&
        std::memcmp(actualHeader, header, sizeof(header)) == 0;
    uint32_t verifiedCrc = 0;
    if (ok) {
        storage::ppg1::Crc32 crc;
        uint8_t chunk[256] {};
        uint32_t remaining = payloadSize;
        while (ok && remaining != 0) {
            const size_t take = std::min<size_t>(remaining, sizeof(chunk));
            ok = readExact(verify, chunk, take);
            if (ok) crc.update(chunk, take);
            remaining -= take;
        }
        verifiedCrc = crc.value();
        ok = ok && verifiedCrc == block.payloadCrc32;
    }
    if (verify) verify.close();
    storage_.unlock();
    const uint32_t durationUs = micros() - startedUs;
    maxSdWriteUs_ = std::max(maxSdWriteUs_, durationUs);
    if (!ok) {
        failSession(PpgSessionError::WriteFailed, nowMs);
        return false;
    }
    streamCrc_.update(blockPayload_, payloadSize);
    ++blockCount_;
    storedSamples_ += sampleCount;
    const storage::PpgJournalState checkpointState =
        static_cast<storage::PpgJournalState>(state_.load()) ==
                storage::PpgJournalState::Finalizing
            ? storage::PpgJournalState::Finalizing
            : storage::PpgJournalState::Recording;
    if (!persistCheckpoint(checkpointState)) {
        failSession(PpgSessionError::FramUnavailable, nowMs);
        return false;
    }
    publishStatus();
    return true;
}

PpgSessionManager::FileScan PpgSessionManager::scanRawFile(
        const char* path) {
    FileScan scan;
    storage_.lock();
    File file = path == nullptr ? File() : SD.open(path, FILE_READ);
    uint8_t fileHeaderBytes[storage::ppg1::FILE_HEADER_SIZE] {};
    if (!file || !readExact(file, fileHeaderBytes, sizeof(fileHeaderBytes))) {
        scan.error = storage::ppg1::Error::TooShort;
        if (file) file.close();
        storage_.unlock();
        return scan;
    }
    scan.error = storage::ppg1::decodeFileHeader(
        fileHeaderBytes, sizeof(fileHeaderBytes), scan.header);
    if (scan.error != storage::ppg1::Error::None) {
        file.close();
        storage_.unlock();
        return scan;
    }
    scan.lastValidOffset = storage::ppg1::FILE_HEADER_SIZE;
    storage::ppg1::Crc32 stream;
    while (file.position() < file.size()) {
        const uint32_t position = file.position();
        uint8_t magic[4] {};
        if (!readExact(file, magic, sizeof(magic))) {
            scan.error = storage::ppg1::Error::TooShort;
            break;
        }
        if (!file.seek(position)) {
            scan.error = storage::ppg1::Error::TooShort;
            break;
        }
        if (std::memcmp(magic, "END1", 4) == 0) {
            uint8_t footerBytes[storage::ppg1::FOOTER_SIZE] {};
            if (!readExact(file, footerBytes, sizeof(footerBytes))) {
                scan.error = storage::ppg1::Error::TooShort;
                break;
            }
            scan.error = storage::ppg1::decodeFooter(
                footerBytes, sizeof(footerBytes), scan.footer);
            if (scan.error != storage::ppg1::Error::None) break;
            if (file.position() != file.size()) {
                scan.error = storage::ppg1::Error::TrailingData;
                break;
            }
            if (scan.footer.blockCount != scan.blockCount ||
                scan.footer.sampleCount != scan.sampleCount ||
                scan.footer.streamCrc32 != stream.value()) {
                scan.error = storage::ppg1::Error::FooterMismatch;
                break;
            }
            scan.complete = true;
            scan.streamCrc32 = stream.value();
            scan.lastValidOffset = file.position();
            scan.error = storage::ppg1::Error::None;
            break;
        }
        uint8_t blockHeaderBytes[storage::ppg1::BLOCK_HEADER_SIZE] {};
        if (!readExact(file, blockHeaderBytes, sizeof(blockHeaderBytes))) {
            scan.error = storage::ppg1::Error::TooShort;
            break;
        }
        storage::ppg1::BlockHeader block;
        scan.error = storage::ppg1::decodeBlockHeader(
            blockHeaderBytes, sizeof(blockHeaderBytes),
            scan.header.blockPayloadMax, block);
        if (scan.error != storage::ppg1::Error::None) break;
        if (block.blockIndex != scan.blockCount) {
            scan.error = storage::ppg1::Error::BlockSequence;
            break;
        }
        if (scan.blockCount != 0 &&
            block.firstSampleIndex < scan.nextLogicalSampleIndex) {
            scan.error = storage::ppg1::Error::SampleSequence;
            break;
        }
        if (block.payloadSize > sizeof(blockPayload_) ||
            !readExact(file, blockPayload_, block.payloadSize)) {
            scan.error = storage::ppg1::Error::TooShort;
            break;
        }
        if (storage::ppg1::crc32(blockPayload_, block.payloadSize) !=
            block.payloadCrc32) {
            scan.error = storage::ppg1::Error::PayloadCrc;
            break;
        }
        stream.update(blockPayload_, block.payloadSize);
        ++scan.blockCount;
        scan.sampleCount += block.sampleCount;
        scan.nextLogicalSampleIndex =
            block.firstSampleIndex + block.sampleCount;
        scan.lastValidOffset = file.position();
        scan.error = storage::ppg1::Error::MissingFooter;
    }
    scan.streamCrc32 = stream.value();
    if (!scan.complete && scan.error == storage::ppg1::Error::None) {
        scan.error = storage::ppg1::Error::MissingFooter;
    }
    file.close();
    storage_.unlock();
    return scan;
}

bool PpgSessionManager::writeMetadata(
        const core::SensorSnapshot& snapshot,
        const char* completionStatus, const char* reason,
        uint64_t endUnixUs, uint32_t rawSize,
        uint32_t finalStreamCrc, uint32_t droppedOverride,
        uint32_t fifoOverflowOverride) {
    char startIso[32] {};
    char endIso[32] {};
    if (!formatIsoUtc(startUnixUs_, startIso, sizeof(startIso)) ||
        !formatIsoUtc(endUnixUs, endIso, sizeof(endIso))) return false;
    const uint32_t driverDrops = nonNegativeDelta(
        snapshot.ppg.droppedSamples, startDriverDrops_);
    const uint32_t fifoOverflows = nonNegativeDelta(
        snapshot.ppg.fifoOverflows, startFifoOverflows_);
    uint32_t ringDrops = 0;
    uint32_t ringHighWater = 0;
    portENTER_CRITICAL(&ringMux_);
    ringDrops = ringDrops_;
    ringHighWater = ringHighWater_;
    portEXIT_CRITICAL(&ringMux_);
    const uint32_t totalDrops = droppedOverride == UINT32_MAX
        ? driverDrops + ringDrops : droppedOverride;
    const uint32_t metadataFifoOverflows =
        fifoOverflowOverride == UINT32_MAX
            ? fifoOverflows : fifoOverflowOverride;

    JsonDocument document;
    document["schema_version"] = 1;
    document["session_id"] = sessionId_;
    document["time_basis"] = "UTC";
    document["start_time"] = startIso;
    document["end_time"] = endIso;
    document["completion"]["status"] = completionStatus;
    document["completion"]["recovered"] =
        std::strncmp(completionStatus, "recovered_", 10) == 0;
    if (reason == nullptr) document["completion"]["reason"] = nullptr;
    else document["completion"]["reason"] = reason;
    document["ppg_format"]["name"] = "PPG1";
    document["ppg_format"]["version"] = 1;
    document["ppg_format"]["endianness"] = "little";
    document["ppg_format"]["sample_encoding"] =
        "uint32_red_uint32_ir";
    document["ppg_format"]["sample_size_bytes"] = 8;
    document["ppg_format"]["block_crc"] = "CRC-32/ISO-HDLC";
    document["max30102"]["sample_rate_hz"] =
        utils::ppg_timing::EFFECTIVE_SAMPLE_RATE_HZ;
    document["max30102"]["sensor_sample_rate_hz"] =
        utils::ppg_timing::SENSOR_SAMPLE_RATE_HZ;
    document["max30102"]["sample_average"] =
        utils::ppg_timing::SAMPLE_AVERAGE;
    document["max30102"]["pulse_width_us"] = 411;
    document["max30102"]["adc_range_na"] = 4096;
    document["max30102"]["red_led_current_ma"] =
        redLedCurrentX10Ma_ / 10.0f;
    document["max30102"]["ir_led_current_ma"] =
        irLedCurrentX10Ma_ / 10.0f;
    if (snapshot.ppg.calculatedValid && !snapshot.ppg.signalPoor) {
        document["result"]["heart_rate_bpm"] = snapshot.ppg.heartRateBpm;
        document["result"]["spo2_percent"] = snapshot.ppg.spo2Percent;
        document["result"]["valid"] = true;
    } else {
        document["result"]["heart_rate_bpm"] = nullptr;
        document["result"]["spo2_percent"] = nullptr;
        document["result"]["valid"] = false;
    }
    document["quality"]["block_count"] = blockCount_;
    document["quality"]["stored_samples"] = storedSamples_;
    document["quality"]["dropped_samples"] = totalDrops;
    document["quality"]["fifo_overflows"] = metadataFifoOverflows;
    const uint64_t expected = static_cast<uint64_t>(storedSamples_) +
        totalDrops;
    document["quality"]["valid_sample_ratio"] = expected == 0
        ? 0.0 : static_cast<double>(storedSamples_) /
            static_cast<double>(expected);
    document["quality"]["ring_high_water_samples"] = ringHighWater;
    document["quality"]["ring_capacity_samples"] = ringCapacity_;
    document["quality"]["ring_memory"] = usingPsram_ ? "PSRAM" : "INTERNAL";
    document["quality"]["max_sd_write_us"] = maxSdWriteUs_;
    document["environment_summary"]["start_recorded"] =
        startEnvironmentRecorded_;
    const uint32_t boundaryRows =
        (startEnvironmentRecorded_ ? 1u : 0u) +
        (endEnvironmentRecorded_ ? 1u : 0u);
    document["environment_summary"]["periodic_records"] =
        environmentRows_ >= boundaryRows
            ? environmentRows_ - boundaryRows
            : 0u;
    document["environment_summary"]["end_recorded"] =
        endEnvironmentRecorded_;
    document["files"]["raw"]["name"] = "raw.ppg";
    document["files"]["raw"]["size_bytes"] = rawSize;
    char crcText[9] {};
    std::snprintf(crcText, sizeof(crcText), "%08lX",
        static_cast<unsigned long>(finalStreamCrc));
    document["files"]["raw"]["stream_crc32"] = crcText;
    document["files"]["environment"]["name"] = "environment.csv";
    document["files"]["environment"]["rows"] = environmentRows_;
    document["firmware"]["storage_schema"] = "FRAM6_CSV7_PPG1";

    storage_.lock();
    if (SD.exists(metadataTmpPath_)) SD.remove(metadataTmpPath_);
    File output = SD.open(metadataTmpPath_, FILE_WRITE);
    const size_t written = output ? serializeJsonPretty(document, output) : 0;
    if (output) {
        output.write('\n');
        output.flush();
        output.close();
    }
    File verify = written > 0 ? SD.open(metadataTmpPath_, FILE_READ) : File();
    JsonDocument actual;
    const DeserializationError parseError = verify
        ? deserializeJson(actual, verify)
        : DeserializationError::EmptyInput;
    if (verify) verify.close();
    const bool ok = !parseError &&
        actual["schema_version"].as<int>() == 1 &&
        std::strcmp(actual["session_id"] | "", sessionId_) == 0 &&
        std::strcmp(actual["completion"]["status"] | "",
                    completionStatus) == 0;
    storage_.unlock();
    return ok;
}

bool PpgSessionManager::verifyAndRename(bool recoveredPartial) {
    (void)recoveredPartial;
    storage_.lock();
    bool ok = !SD.exists(rawPath_) && !SD.exists(environmentPath_) &&
        !SD.exists(metadataPath_) && SD.exists(rawTmpPath_) &&
        SD.exists(environmentTmpPath_) && SD.exists(metadataTmpPath_);
    if (ok) ok = SD.rename(rawTmpPath_, rawPath_);
    if (ok) ok = SD.rename(environmentTmpPath_, environmentPath_);
    if (ok) ok = SD.rename(metadataTmpPath_, metadataPath_);
    storage_.unlock();
    if (!ok) return false;
    const FileScan scan = scanRawFile(rawPath_);
    if (!scan.complete || scan.error != storage::ppg1::Error::None) {
        return false;
    }
    storage_.lock();
    File metadata = SD.open(metadataPath_, FILE_READ);
    JsonDocument document;
    const DeserializationError error = metadata
        ? deserializeJson(document, metadata)
        : DeserializationError::EmptyInput;
    if (metadata) metadata.close();
    const bool metadataValid = !error &&
        std::strcmp(document["session_id"] | "", sessionId_) == 0;
    storage_.unlock();
    return metadataValid;
}

bool PpgSessionManager::finalizeSession(
        const core::SensorSnapshot& snapshot, uint32_t nowMs,
        bool recovered, bool recoveredPartial) {
    capturing_ = false;
    if (!persistCheckpoint(storage::PpgJournalState::Finalizing)) {
        failSession(PpgSessionError::FramUnavailable, nowMs);
        return false;
    }
    uint32_t firstSampleIndex = 0;
    uint16_t sampleCount = 0;
    while (popBlock(firstSampleIndex, sampleCount, true)) {
        if (!appendBlock(firstSampleIndex, sampleCount, nowMs)) return false;
    }
    endEnvironmentRecorded_ = appendEnvironment(snapshot, "END", nowMs);

    const core::TimeSnapshot clock = hal::Clock::snapshot();
    const uint64_t endUnixUs = clock.utcValid && clock.utcEpochUs > 0
        ? static_cast<uint64_t>(clock.utcEpochUs)
        : startUnixUs_ + static_cast<uint64_t>(
            elapsed(nowMs, startMonotonicMs_)) * 1000u;
    uint32_t ringDrops = 0;
    portENTER_CRITICAL(&ringMux_);
    ringDrops = ringDrops_;
    portEXIT_CRITICAL(&ringMux_);
    const uint32_t totalDrops = ringDrops + nonNegativeDelta(
        snapshot.ppg.droppedSamples, startDriverDrops_);
    const uint32_t fifoOverflows = nonNegativeDelta(
        snapshot.ppg.fifoOverflows, startFifoOverflows_);
    storage::ppg1::Footer footer;
    footer.endUnixUs = endUnixUs;
    footer.blockCount = blockCount_;
    footer.sampleCount = storedSamples_;
    footer.droppedSamples = totalDrops;
    footer.fifoOverflows = fifoOverflows;
    footer.streamCrc32 = streamCrc_.value();
    uint8_t encoded[storage::ppg1::FOOTER_SIZE] {};
    storage::ppg1::encodeFooter(footer, encoded);

    storage_.lock();
    File raw = SD.open(rawTmpPath_, FILE_APPEND);
    bool ok = raw && writeExact(raw, encoded, sizeof(encoded));
    if (raw) {
        raw.flush();
        raw.close();
    }
    storage_.unlock();
    if (!ok) {
        failSession(PpgSessionError::WriteFailed, nowMs);
        return false;
    }
    const FileScan scan = scanRawFile(rawTmpPath_);
    if (!scan.complete || scan.error != storage::ppg1::Error::None ||
        scan.blockCount != blockCount_ || scan.sampleCount != storedSamples_) {
        failSession(PpgSessionError::VerifyFailed, nowMs);
        return false;
    }
    const char* completion = recoveredPartial ? "recovered_partial"
        : (recovered ? "recovered_complete"
                     : (storedSamples_ == 0 ? "aborted" : "complete"));
    const char* reason = recoveredPartial ? "interrupted_write"
        : (recovered ? "interrupted_finalize"
                     : (storedSamples_ == 0 ? "no_samples" : nullptr));
    if (!writeMetadata(snapshot, completion, reason, endUnixUs,
                       static_cast<uint32_t>(scan.lastValidOffset),
                       scan.streamCrc32) ||
        !verifyAndRename(recoveredPartial)) {
        failSession(PpgSessionError::RenameFailed, nowMs);
        return false;
    }
    const storage::PpgJournalState finalState = storedSamples_ == 0
        ? storage::PpgJournalState::Aborted
        : (recoveredPartial
            ? storage::PpgJournalState::CommittedPartial
            : storage::PpgJournalState::Committed);
    persistCheckpoint(finalState);
    storage_.appendEvent(
        storedSamples_ == 0 ? storage::EventCode::PpgSessionAborted
                            : storage::EventCode::PpgSessionCompleted,
        static_cast<int32_t>(storedSamples_), nowMs);
    storage_.clearPpgCheckpoint();
    setState(finalState);
    lastError_ = storedSamples_ == 0
        ? PpgSessionError::NoSamples : PpgSessionError::None;
    publishStatus(totalDrops, fifoOverflows);
    Logger::info("PpgSession",
        "Session finalized: %s samples=%lu drops=%lu",
        sessionId_, static_cast<unsigned long>(storedSamples_),
        static_cast<unsigned long>(totalDrops));
    return true;
}

void PpgSessionManager::failSession(PpgSessionError error,
                                    uint32_t nowMs) {
    capturing_ = false;
    lastError_ = error;
    bool recoverableArtifact = false;
    if (error != PpgSessionError::PathCollision && startUnixUs_ != 0 &&
        storage_.isSdAvailable()) {
        storage_.lock();
        recoverableArtifact = SD.exists(rawTmpPath_) || SD.exists(rawPath_);
        storage_.unlock();
    }
    if (recoverableArtifact && storage_.isPpgJournalAvailable()) {
        persistCheckpoint(storage::PpgJournalState::RecoveryPending);
        setState(storage::PpgJournalState::RecoveryPending);
        recoveryNeeded_ = true;
    } else if (error == PpgSessionError::PathCollision) {
        // IDs have one-second resolution. Preserve the existing directory and
        // retry after the UTC second changes.
        storage_.clearPpgCheckpoint();
        startUnixUs_ = 0;
        nextStartAttemptMs_ = nowMs + 1000u;
        setState(storage::PpgJournalState::Empty);
    } else {
        setState(storage::PpgJournalState::Failed);
    }
    storage_.appendEvent(storage::EventCode::PpgStorageFailed,
        static_cast<int32_t>(error), nowMs);
    publishStatus();
    Logger::error("PpgSession", "Session failure: %s",
                  ppgSessionErrorName(error));
}

bool PpgSessionManager::copyValidRawPrefix(
        const char* sourcePath, const char* recoveryPath,
        const FileScan& scan, uint64_t endUnixUs,
        uint32_t droppedSamples, uint32_t fifoOverflows) {
    if (scan.blockCount == 0 || scan.lastValidOffset <=
        storage::ppg1::FILE_HEADER_SIZE) return false;
    storage::ppg1::Footer footer;
    footer.endUnixUs = endUnixUs;
    footer.blockCount = scan.blockCount;
    footer.sampleCount = scan.sampleCount;
    footer.droppedSamples = droppedSamples;
    footer.fifoOverflows = fifoOverflows;
    footer.streamCrc32 = scan.streamCrc32;
    uint8_t footerBytes[storage::ppg1::FOOTER_SIZE] {};
    storage::ppg1::encodeFooter(footer, footerBytes);
    uint8_t copyBuffer[512] {};

    storage_.lock();
    if (SD.exists(recoveryPath)) {
        storage_.unlock();
        return false;
    }
    File source = SD.open(sourcePath, FILE_READ);
    File target = SD.open(recoveryPath, FILE_WRITE);
    bool ok = source && target;
    size_t remaining = scan.lastValidOffset;
    while (ok && remaining != 0) {
        const size_t take = std::min(remaining, sizeof(copyBuffer));
        ok = readExact(source, copyBuffer, take) &&
             writeExact(target, copyBuffer, take);
        remaining -= take;
    }
    ok = ok && writeExact(target, footerBytes, sizeof(footerBytes));
    if (target) {
        target.flush();
        target.close();
    }
    if (source) source.close();
    storage_.unlock();
    if (!ok) return false;
    const FileScan recovered = scanRawFile(recoveryPath);
    return recovered.complete &&
        recovered.error == storage::ppg1::Error::None &&
        recovered.blockCount == scan.blockCount &&
        recovered.sampleCount == scan.sampleCount;
}

bool PpgSessionManager::recoverEnvironmentFile() {
    storage_.lock();
    auto inspect = [](const char* path, uint32_t& rows,
                      bool& completeTail, uint32_t& lastNewline) {
        rows = 0;
        completeTail = false;
        lastNewline = 0;
        File file = SD.open(path, FILE_READ);
        if (!file || file.size() < std::strlen(ENV_HEADER) + 1) {
            if (file) file.close();
            return false;
        }
        char header[192] {};
        const size_t expectedHeader = std::strlen(ENV_HEADER);
        bool ok = file.read(reinterpret_cast<uint8_t*>(header),
                            expectedHeader + 1) == expectedHeader + 1 &&
            std::memcmp(header, ENV_HEADER, expectedHeader) == 0 &&
            header[expectedHeader] == '\n';
        uint32_t position = static_cast<uint32_t>(expectedHeader + 1);
        lastNewline = position;
        uint8_t buffer[256] {};
        uint8_t lastByte = '\n';
        while (ok && file.available()) {
            const size_t count = file.read(buffer, sizeof(buffer));
            if (count == 0) break;
            for (size_t i = 0; i < count; ++i) {
                if (buffer[i] == '\n') {
                    ++rows;
                    lastNewline = position + i + 1;
                }
            }
            lastByte = buffer[count - 1];
            position += count;
        }
        completeTail = ok && lastByte == '\n';
        file.close();
        return ok;
    };

    if (SD.exists(environmentPath_)) {
        uint32_t rows = 0;
        uint32_t lastNewline = 0;
        bool completeTail = false;
        const bool valid = inspect(environmentPath_, rows,
                                   completeTail, lastNewline) && completeTail;
        if (valid) environmentRows_ = rows;
        storage_.unlock();
        return valid;
    }
    if (!SD.exists(environmentTmpPath_)) {
        File created = SD.open(environmentPath_, FILE_WRITE);
        const bool ok = created && writeExact(created,
            reinterpret_cast<const uint8_t*>(ENV_HEADER),
            std::strlen(ENV_HEADER)) &&
            writeExact(created, reinterpret_cast<const uint8_t*>("\n"), 1);
        if (created) {
            created.flush();
            created.close();
        }
        storage_.unlock();
        return ok;
    }
    uint32_t rows = 0;
    uint32_t lastNewline = 0;
    bool completeTail = false;
    if (!inspect(environmentTmpPath_, rows, completeTail, lastNewline)) {
        storage_.unlock();
        return false;
    }
    if (completeTail) {
        const bool ok = SD.rename(environmentTmpPath_, environmentPath_);
        if (ok) environmentRows_ = rows;
        storage_.unlock();
        return ok;
    }

    if (lastNewline == 0) {
        storage_.unlock();
        return false;
    }
    char recoveryPath[112] {};
    char incompletePath[112] {};
    std::snprintf(recoveryPath, sizeof(recoveryPath),
                  "%s.recover.tmp", environmentPath_);
    std::snprintf(incompletePath, sizeof(incompletePath),
                  "%s.incomplete", environmentPath_);
    if (SD.exists(recoveryPath) || SD.exists(incompletePath)) {
        storage_.unlock();
        return false;
    }
    File source = SD.open(environmentTmpPath_, FILE_READ);
    File target = SD.open(recoveryPath, FILE_WRITE);
    bool ok = source && target;
    uint8_t buffer[256] {};
    uint32_t remaining = lastNewline;
    while (ok && remaining != 0) {
        const size_t take = std::min<size_t>(remaining, sizeof(buffer));
        ok = readExact(source, buffer, take) &&
             writeExact(target, buffer, take);
        remaining -= take;
    }
    if (target) {
        target.flush();
        target.close();
    }
    if (source) source.close();
    ok = ok && SD.rename(environmentTmpPath_, incompletePath) &&
         SD.rename(recoveryPath, environmentPath_);
    if (ok) environmentRows_ = rows;
    storage_.unlock();
    return ok;
}

bool PpgSessionManager::recoverPending(
        const core::SensorSnapshot& snapshot, uint32_t nowMs) {
    storage::FramPpgCheckpoint checkpoint {};
    if (!storage_.getPpgCheckpoint(checkpoint) ||
        checkpoint.startUnixUs == 0 ||
        !buildPaths(checkpoint.startUnixUs)) {
        lastError_ = PpgSessionError::RecoveryFailed;
        return false;
    }
    startUnixUs_ = checkpoint.startUnixUs;
    startMonotonicMs_ = nowMs;
    startDriverDrops_ = snapshot.ppg.droppedSamples;
    startFifoOverflows_ = snapshot.ppg.fifoOverflows;
    environmentRows_ = 0;
    startEnvironmentRecorded_ = false;
    endEnvironmentRecorded_ = false;
    maxSdWriteUs_ = 0;

    storage_.lock();
    const bool rawFinalExists = SD.exists(rawPath_);
    const bool rawTmpExists = SD.exists(rawTmpPath_);
    const bool metadataFinalExists = SD.exists(metadataPath_);
    const bool environmentFinalExists = SD.exists(environmentPath_);
    storage_.unlock();
    const char* sourcePath = rawFinalExists ? rawPath_
        : (rawTmpExists ? rawTmpPath_ : nullptr);
    if (sourcePath == nullptr) {
        lastError_ = PpgSessionError::RecoveryFailed;
        return false;
    }
    FileScan scan = scanRawFile(sourcePath);
    if (scan.error != storage::ppg1::Error::None && scan.blockCount == 0) {
        lastError_ = PpgSessionError::RecoveryFailed;
        return false;
    }
    if (scan.complete && metadataFinalExists && environmentFinalExists) {
        storage_.lock();
        File metadata = SD.open(metadataPath_, FILE_READ);
        JsonDocument document;
        const DeserializationError metadataError = metadata
            ? deserializeJson(document, metadata)
            : DeserializationError::EmptyInput;
        if (metadata) metadata.close();
        const bool metadataValid = !metadataError &&
            std::strcmp(document["session_id"] | "", sessionId_) == 0 &&
            document["quality"]["block_count"].as<uint32_t>() ==
                scan.blockCount &&
            document["quality"]["stored_samples"].as<uint32_t>() ==
                scan.sampleCount;
        storage_.unlock();
        if (!metadataValid) {
            lastError_ = PpgSessionError::RecoveryFailed;
            return false;
        }
        if (!recoverEnvironmentFile()) {
            lastError_ = PpgSessionError::RecoveryFailed;
            return false;
        }
        blockCount_ = scan.blockCount;
        storedSamples_ = scan.sampleCount;
        storage_.clearPpgCheckpoint();
        setState(storage::PpgJournalState::Committed);
        recoveryNeeded_ = false;
        lastError_ = PpgSessionError::None;
        publishStatus(scan.footer.droppedSamples, scan.footer.fifoOverflows);
        return true;
    }
    if (metadataFinalExists) {
        // metadata.json is the commit marker. An inconsistent committed marker
        // must never be overwritten automatically.
        lastError_ = PpgSessionError::RecoveryFailed;
        return false;
    }

    bool partial = !scan.complete;
    const core::TimeSnapshot clock = hal::Clock::snapshot();
    const uint64_t endUnixUs = clock.utcValid && clock.utcEpochUs > 0
        ? static_cast<uint64_t>(clock.utcEpochUs) : startUnixUs_;
    if (partial) {
        char recoveryRaw[112] {};
        char incompleteRaw[112] {};
        std::snprintf(recoveryRaw, sizeof(recoveryRaw),
                      "%s.recover.tmp", rawPath_);
        std::snprintf(incompleteRaw, sizeof(incompleteRaw),
                      "%s.incomplete", rawPath_);
        if (!copyValidRawPrefix(sourcePath, recoveryRaw, scan, endUnixUs,
                                checkpoint.droppedSamples,
                                checkpoint.fifoOverflows)) {
            lastError_ = PpgSessionError::RecoveryFailed;
            return false;
        }
        storage_.lock();
        bool renamed = !SD.exists(incompleteRaw);
        if (renamed) renamed = SD.rename(sourcePath, incompleteRaw);
        if (renamed) renamed = SD.rename(recoveryRaw, rawPath_);
        storage_.unlock();
        if (!renamed) {
            lastError_ = PpgSessionError::RenameFailed;
            return false;
        }
        scan = scanRawFile(rawPath_);
    } else if (!rawFinalExists) {
        storage_.lock();
        const bool renamed = !SD.exists(rawPath_) &&
            SD.rename(rawTmpPath_, rawPath_);
        storage_.unlock();
        if (!renamed) {
            lastError_ = PpgSessionError::RenameFailed;
            return false;
        }
    }
    if (!scan.complete || !recoverEnvironmentFile()) {
        lastError_ = PpgSessionError::RecoveryFailed;
        return false;
    }
    blockCount_ = scan.blockCount;
    storedSamples_ = scan.sampleCount;
    if (!writeMetadata(snapshot,
            partial ? "recovered_partial" : "recovered_complete",
            partial ? "interrupted_write" : "interrupted_finalize",
            scan.footer.endUnixUs != 0 ? scan.footer.endUnixUs : endUnixUs,
            static_cast<uint32_t>(scan.lastValidOffset),
            scan.streamCrc32, scan.footer.droppedSamples,
            scan.footer.fifoOverflows)) {
        lastError_ = PpgSessionError::WriteFailed;
        return false;
    }
    storage_.lock();
    bool metadataRenamed = false;
    if (SD.exists(metadataPath_)) {
        metadataRenamed = metadataFinalExists;
    } else {
        metadataRenamed = SD.rename(metadataTmpPath_, metadataPath_);
    }
    storage_.unlock();
    if (!metadataRenamed) {
        lastError_ = PpgSessionError::RenameFailed;
        return false;
    }
    persistCheckpoint(partial
        ? storage::PpgJournalState::CommittedPartial
        : storage::PpgJournalState::Committed);
    storage_.appendEvent(partial
        ? storage::EventCode::PpgSessionRecoveredPartial
        : storage::EventCode::PpgSessionCompleted,
        static_cast<int32_t>(storedSamples_), nowMs);
    storage_.clearPpgCheckpoint();
    setState(partial ? storage::PpgJournalState::CommittedPartial
                     : storage::PpgJournalState::Committed);
    recoveryNeeded_ = false;
    lastError_ = PpgSessionError::None;
    publishStatus(scan.footer.droppedSamples, scan.footer.fifoOverflows);
    return true;
}

void PpgSessionManager::update(const core::SensorSnapshot& snapshot,
                               uint32_t nowMs) {
    latestDriverDrops_ = snapshot.ppg.droppedSamples;
    latestFifoOverflows_ = snapshot.ppg.fifoOverflows;
    uint32_t currentRingDrops = 0;
    portENTER_CRITICAL(&ringMux_);
    currentRingDrops = ringDrops_;
    portEXIT_CRITICAL(&ringMux_);
    publishStatus(currentRingDrops + nonNegativeDelta(
        latestDriverDrops_, startDriverDrops_), nonNegativeDelta(
        latestFifoOverflows_, startFifoOverflows_));
    if (recoveryNeeded_) {
        if (!storage_.isSdAvailable()) return;
        if (!recoverPending(snapshot, nowMs)) {
            persistCheckpoint(storage::PpgJournalState::RecoveryFailed);
            setState(storage::PpgJournalState::RecoveryFailed);
            storage_.appendEvent(
                storage::EventCode::PpgSessionRecoveryFailed,
                static_cast<int32_t>(lastError_), nowMs);
            recoveryNeeded_ = false;
            publishStatus();
        }
        return;
    }

    const storage::PpgJournalState current =
        static_cast<storage::PpgJournalState>(state_.load());
    if ((current == storage::PpgJournalState::Committed ||
         current == storage::PpgJournalState::CommittedPartial ||
         current == storage::PpgJournalState::Aborted ||
         current == storage::PpgJournalState::Failed) &&
        !desiredRecording_.load()) {
        setState(storage::PpgJournalState::Empty);
        std::memset(sessionId_, 0, sizeof(sessionId_));
        publishStatus();
        return;
    }
    if (current == storage::PpgJournalState::Empty &&
        desiredRecording_.load() &&
        static_cast<int32_t>(nowMs - nextStartAttemptMs_) >= 0) {
        startSession(snapshot, nowMs);
        return;
    }
    if (current != storage::PpgJournalState::Recording) return;

    uint32_t firstSampleIndex = 0;
    uint16_t sampleCount = 0;
    if (popBlock(firstSampleIndex, sampleCount, false)) {
        appendBlock(firstSampleIndex, sampleCount, nowMs);
    }
    if (elapsed(nowMs, lastEnvironmentMs_) >= 1000u) {
        lastEnvironmentMs_ = nowMs;
        if (!appendEnvironment(snapshot, "PERIODIC", nowMs)) {
            storage_.appendEvent(storage::EventCode::PpgStorageFailed,
                static_cast<int32_t>(PpgSessionError::WriteFailed), nowMs);
        }
    }
    uint32_t ringDrops = 0;
    portENTER_CRITICAL(&ringMux_);
    ringDrops = ringDrops_;
    portEXIT_CRITICAL(&ringMux_);
    if (ringDrops != lastDropEventCount_ &&
        (lastDropEventMs_ == 0 ||
         elapsed(nowMs, lastDropEventMs_) >= 60000u)) {
        lastDropEventCount_ = ringDrops;
        lastDropEventMs_ = nowMs;
        storage_.appendEvent(storage::EventCode::PpgRingDrop,
            static_cast<int32_t>(ringDrops), nowMs);
    }
    if (!desiredRecording_.load()) {
        finalizeSession(snapshot, nowMs, false, false);
    }
}

} // namespace services
