#include "storage/storage_manager.h"
#include "services/logger.h"
#include "hal/pins.h"
#include "hal/clock.h"
#include "storage/csv_v7.h"
#include "storage/storage_record_codec.h"
#include <Arduino.h>
#include <cmath>
#include <cstring>

namespace storage {

StorageManager::StorageManager()
    : wal_(fram_), sdTransaction_(fram_), ppgJournal_(fram_),
      sdAvailable_(false), framAvailable_(false),
      lastSdInitAttempt_(0) {
    mutex_ = xSemaphoreCreateRecursiveMutex();
}

void StorageManager::forceFlush() {
    if (!framAvailable_) return;
    
    lock();
    bool wasWifiActive = wifiActive_;
    wifiActive_ = false;
    unlock();
    
    // フラッシュ処理（内部でロックとローテーション、SD書き込みが行われる）
    flushPendingToSd();
    
    lock();
    wifiActive_ = wasWifiActive;
    unlock();
}

void StorageManager::lock() const {
    if (mutex_) {
        xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
    }
}

void StorageManager::unlock() const {
    if (mutex_) {
        xSemaphoreGiveRecursive(mutex_);
    }
}

uint16_t StorageManager::calculateCrc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

bool StorageManager::begin() {
    services::Logger::info("StorageMgr", "Initializing StorageManager...");

    framAvailable_ = fram_.begin();
    if (framAvailable_) {
        initSuperblock();
        if (!loadSuperblock()) {
            framAvailable_ = false;
            framReadOnly_ = true;
            services::Logger::error("StorageMgr",
                "FRAM metadata is not safely writable; preserving all contents read-only");
        } else {
            superblock_.bootCount++;
            if (!saveSuperblock()) {
                framAvailable_ = false;
                framReadOnly_ = true;
                services::Logger::error("StorageMgr",
                    "Failed to commit boot checkpoint; preserving FRAM read-only");
            } else {
                const FramWalStatus txStatus =
                    sdTransaction_.begin(superblock_.lastSdFlushSequence);
                if (txStatus != FramWalStatus::Ready) {
                    framAvailable_ = false;
                    framReadOnly_ = true;
                    services::Logger::error("StorageMgr",
                        "SD transaction journal is corrupt; preserving FRAM read-only");
                } else if (sdTransaction_.active()) {
                    currentFilename_ = sdTransaction_.current().filename;
                    services::Logger::warn("StorageMgr",
                        "Resuming SD transaction seq=%lu path=%s",
                        static_cast<unsigned long>(sdTransaction_.current().sequence),
                        currentFilename_.c_str());
                }
                if (framAvailable_) {
                    const FramWalStatus ppgStatus = ppgJournal_.begin();
                    ppgJournalAvailable_ = ppgStatus == FramWalStatus::Ready;
                    if (!ppgJournalAvailable_) {
                        services::Logger::error("StorageMgr",
                            "PPG checkpoint journal unavailable; PPG sessions disabled without altering other FRAM data");
                    }
                }
                services::Logger::info("StorageMgr",
                    "FRAM WAL loaded. boot=%u pending=%u dropped=%lu high_water=%u",
                    superblock_.bootCount, getPendingCount(),
                    static_cast<unsigned long>(walStats_.droppedRecords),
                    walStats_.highWaterRecords);
            }
        }

    }
    if (framAvailable_) {
        loadEventJournal();
        appendEvent(EventCode::Boot, static_cast<int32_t>(superblock_.bootCount), millis());
    }

    initSdCard();

    return framAvailable_ || sdAvailable_;
}

bool StorageManager::getPpgCheckpoint(
        FramPpgCheckpoint& checkpoint) const {
    lock();
    const bool available = framAvailable_ && ppgJournalAvailable_ &&
        ppgJournal_.initialized();
    if (available) checkpoint = ppgJournal_.current();
    unlock();
    return available;
}

bool StorageManager::savePpgCheckpoint(
        const FramPpgCheckpoint& checkpoint) {
    if (!framAvailable_ || !ppgJournalAvailable_) return false;
    lock();
    const bool saved = ppgJournal_.persist(checkpoint) == FramWalStatus::Ready;
    unlock();
    return saved;
}

bool StorageManager::clearPpgCheckpoint() {
    if (!framAvailable_ || !ppgJournalAvailable_) return false;
    lock();
    const bool saved = ppgJournal_.clear() == FramWalStatus::Ready;
    unlock();
    return saved;
}

void StorageManager::initSuperblock() {
    superblock_.magic = FRAM_MAGIC;
    superblock_.formatVersion = FRAM_FORMAT_VERSION;
    superblock_.writeIndex = 0;
    superblock_.readIndex = 0;
    superblock_.nextSequence = 1;
    superblock_.bootCount = 0;
    superblock_.lastSdFlushSequence = 0;
    
    superblock_.hasValidSgp41State = false;
    superblock_.sgp41VocState0 = 0;
    superblock_.sgp41VocState1 = 0;

    // 起算日: 2026-08-15 00:00:00 UTC (1786752000)
    superblock_.lastScd41CalibrationEpoch = 1786752000;
    
    superblock_.hasValidBmp581Calibration = false;
    superblock_.bmp581PressureOffsetHpa = 0.0f;
    superblock_.bmp581CalibEpoch = 0;
    superblock_.bmp581CalibTempC = NAN;
    superblock_.bmp581CalibSeaLevelHpa = NAN;
}

bool StorageManager::getSgp41States(float& voc0, float& voc1) const {
    lock();
    const bool valid = framAvailable_ && superblock_.hasValidSgp41State;
    if (valid) {
        voc0 = superblock_.sgp41VocState0;
        voc1 = superblock_.sgp41VocState1;
    }
    unlock();
    return valid;
}

void StorageManager::setSgp41States(float voc0, float voc1) {
    if (!framAvailable_) return;
    
    lock();
    superblock_.hasValidSgp41State = true;
    superblock_.sgp41VocState0 = voc0;
    superblock_.sgp41VocState1 = voc1;
    saveSuperblock();
    unlock();
}

uint32_t StorageManager::getScd41LastCalibrationEpoch() const {
    lock();
    const uint32_t epoch = framAvailable_ ? superblock_.lastScd41CalibrationEpoch : 0;
    unlock();
    return epoch;
}

void StorageManager::setScd41LastCalibrationEpoch(uint32_t epoch) {
    if (!framAvailable_) return;
    
    lock();
    superblock_.lastScd41CalibrationEpoch = epoch;
    saveSuperblock();
    unlock();
}

bool StorageManager::appendAmedasLogLine(const char* line) {
    static constexpr const char* PATH = "/amedas_pressure_v1.csv";
    static constexpr const char* HEADER =
        "UpdateUtcEpochMs,ObservationUtcEpochMs,LocationSource,LatitudeDeg,LongitudeDeg,PreviousP0Hpa,InterpolatedP0Hpa,PressureFieldState,Result,CachedStations,UsedStations,"
        "Station1Id,Station1DistanceKm,Station1PressureHpa,Station1Quality,Station1Used,"
        "Station2Id,Station2DistanceKm,Station2PressureHpa,Station2Quality,Station2Used,"
        "Station3Id,Station3DistanceKm,Station3PressureHpa,Station3Quality,Station3Used,"
        "Station4Id,Station4DistanceKm,Station4PressureHpa,Station4Quality,Station4Used,"
        "Station5Id,Station5DistanceKm,Station5PressureHpa,Station5Quality,Station5Used";
    return appendVerifiedDiagnosticCsv(PATH, HEADER, line);
}

bool StorageManager::appendBmp581CalibrationLogLine(const char* line) {
    static constexpr const char* PATH = "/bmp581_calibration_v1.csv";
    static constexpr const char* HEADER =
        "FormatVersion,FirmwareSchema,StartUtcEpoch,EndUtcEpoch,Sensor,ReferenceAltitudeM,SettleSeconds,CollectionSeconds,TotalSamples,ValidSamples,UsedSamples,ExcludedSamples,TemperatureSource,MeanTemperatureC,MeanSeaLevelPressureHpa,MeanRawPressureHpa,MeanExpectedPressureHpa,CandidateOffsetHpa,PreMeanAltitudeM,PreStdDevM,PostMeanAltitudeM,PostStdDevM,Accepted,PersistenceVerified,Failure";
    return appendVerifiedDiagnosticCsv(PATH, HEADER, line);
}

bool StorageManager::appendVerifiedDiagnosticCsv(
        const char* path, const char* header, const char* line) {
    constexpr size_t MAX_LINE = 768;
    if (!sdAvailable_ || path == nullptr || header == nullptr ||
        line == nullptr) return false;
    const size_t lineLength = std::strlen(line);
    const size_t headerLength = std::strlen(header);
    if (lineLength == 0 || lineLength >= MAX_LINE ||
        headerLength == 0 || headerLength >= MAX_LINE ||
        std::strchr(line, '\n') != nullptr ||
        std::strchr(line, '\r') != nullptr ||
        std::strchr(header, '\n') != nullptr ||
        std::strchr(header, '\r') != nullptr) {
        return false;
    }

    lock();
    bool ok = true;
    if (!SD.exists(path)) {
        File create = SD.open(path, FILE_WRITE);
        const size_t wroteHeader = create
            ? create.write(reinterpret_cast<const uint8_t*>(header),
                           headerLength) : 0;
        const size_t wroteNewline = wroteHeader == headerLength
            ? create.write(reinterpret_cast<const uint8_t*>("\n"), 1) : 0;
        if (create) {
            create.flush();
            create.close();
        }
        ok = wroteHeader == headerLength && wroteNewline == 1;
    }

    uint32_t originalSize = 0;
    if (ok) {
        File inspect = SD.open(path, FILE_READ);
        ok = static_cast<bool>(inspect);
        if (ok) {
            originalSize = inspect.size();
            char headerActual[MAX_LINE] {};
            ok = originalSize >= headerLength + 1 && inspect.seek(0) &&
                inspect.read(reinterpret_cast<uint8_t*>(headerActual),
                             headerLength + 1) == headerLength + 1 &&
                std::memcmp(headerActual, header, headerLength) == 0 &&
                headerActual[headerLength] == '\n';
            if (originalSize > 0) {
                ok = ok && inspect.seek(originalSize - 1);
                const int lastByte = ok ? inspect.read() : -1;
                if (ok && lastByte != '\n') {
                    inspect.close();
                    File repair = SD.open(path, FILE_APPEND);
                    ok = static_cast<bool>(repair) &&
                        repair.write(reinterpret_cast<const uint8_t*>("\n"), 1) == 1;
                    if (repair) {
                        repair.flush();
                        repair.close();
                    }
                    if (ok) ++originalSize;
                }
            }
            if (inspect) inspect.close();
        }
    }

    if (ok) {
        File output = SD.open(path, FILE_APPEND);
        const size_t wroteLine = output
            ? output.write(reinterpret_cast<const uint8_t*>(line), lineLength)
            : 0;
        const size_t wroteNewline = wroteLine == lineLength
            ? output.write(reinterpret_cast<const uint8_t*>("\n"), 1) : 0;
        if (output) {
            output.flush();
            output.close();
        }
        ok = wroteLine == lineLength && wroteNewline == 1;
    }

    if (ok) {
        File verify = SD.open(path, FILE_READ);
        ok = static_cast<bool>(verify) &&
            verify.size() == originalSize + lineLength + 1 &&
            verify.seek(originalSize);
        char actual[MAX_LINE] {};
        const size_t expected = lineLength + 1;
        const size_t readBytes = ok
            ? verify.read(reinterpret_cast<uint8_t*>(actual), expected) : 0;
        if (verify) verify.close();
        ok = ok && readBytes == expected && actual[lineLength] == '\n' &&
            std::memcmp(actual, line, lineLength) == 0;
    }
    unlock();
    return ok;
}

bool StorageManager::getBmp581Calibration(float& offsetHpa, uint32_t& epoch, float& tempC, float& slpHpa) const {
    lock();
    const bool valid = framAvailable_ &&
        superblock_.hasValidBmp581Calibration &&
        std::isfinite(superblock_.bmp581PressureOffsetHpa) &&
        std::fabs(superblock_.bmp581PressureOffsetHpa) <= 5.0f &&
        superblock_.bmp581CalibEpoch != 0 &&
        std::isfinite(superblock_.bmp581CalibTempC) &&
        superblock_.bmp581CalibTempC >= -40.0f &&
        superblock_.bmp581CalibTempC <= 85.0f &&
        std::isfinite(superblock_.bmp581CalibSeaLevelHpa) &&
        superblock_.bmp581CalibSeaLevelHpa > 800.0f &&
        superblock_.bmp581CalibSeaLevelHpa < 1200.0f;
    if (valid) {
        offsetHpa = superblock_.bmp581PressureOffsetHpa;
        epoch = superblock_.bmp581CalibEpoch;
        tempC = superblock_.bmp581CalibTempC;
        slpHpa = superblock_.bmp581CalibSeaLevelHpa;
    }
    unlock();
    return valid;
}

bool StorageManager::setBmp581Calibration(float offsetHpa, uint32_t epoch, float tempC, float slpHpa) {
    if (!framAvailable_ || !std::isfinite(offsetHpa) ||
        std::fabs(offsetHpa) > 5.0f || epoch == 0 ||
        !std::isfinite(tempC) || tempC < -40.0f || tempC > 85.0f ||
        !std::isfinite(slpHpa) || slpHpa <= 800.0f || slpHpa >= 1200.0f) {
        return false;
    }
    
    lock();
    const FramSuperblock previous = superblock_;
    superblock_.hasValidBmp581Calibration = true;
    superblock_.bmp581PressureOffsetHpa = offsetHpa;
    superblock_.bmp581CalibEpoch = epoch;
    superblock_.bmp581CalibTempC = tempC;
    superblock_.bmp581CalibSeaLevelHpa = slpHpa;
    const bool saved = saveSuperblock();
    if (!saved) superblock_ = previous;
    unlock();
    return saved;
}

bool StorageManager::loadSuperblock() {
    const FramSuperblock initial = superblock_;
    const FramWalStatus status = wal_.begin(superblock_, walStats_, initial);
    if (status == FramWalStatus::Incompatible) {
        services::Logger::error("StorageMgr", "Unsupported FRAM format; automatic initialization refused");
    } else if (status == FramWalStatus::Corrupt) {
        services::Logger::error("StorageMgr", "FRAM checkpoint corrupt; automatic initialization refused");
    } else if (status != FramWalStatus::Ready) {
        services::Logger::error("StorageMgr", "FRAM checkpoint I/O failed");
    }
    return status == FramWalStatus::Ready;
}

bool StorageManager::saveSuperblock() {
    if (!framAvailable_) return false;
    return wal_.persist(superblock_, walStats_) == FramWalStatus::Ready;
}

uint16_t StorageManager::getPendingCount() const {
    lock();
    const uint16_t pending = FramWal<FramStorage>::pendingCount(superblock_);
    unlock();
    return pending;
}

FramWalStats StorageManager::getWalStats() const {
    lock();
    const FramWalStats stats = walStats_;
    unlock();
    return stats;
}

uint16_t StorageManager::getEventCount() const {
    lock();
    const uint16_t count = eventCount_;
    unlock();
    return count;
}

String StorageManager::getCurrentFilename() const {
    lock();
    const String filename = currentFilename_;
    unlock();
    return filename;
}

bool StorageManager::readEventSlot(uint16_t index, EventRecord& record) {
    if (!framAvailable_ || index >= MAX_EVENT_RECORDS) return false;

    memset(&record, 0, sizeof(record));
    uint16_t addr = ADDR_EVENT_JOURNAL + (index * EVENT_SLOT_SIZE);
    if (!fram_.read(addr, reinterpret_cast<uint8_t*>(&record), sizeof(record))) return false;

    if (record.header.committed != 1 ||
        record.header.length != EVENT_PAYLOAD_SIZE ||
        record.header.sequence == 0) {
        return false;
    }

    uint16_t code = record.eventCode;
    bool knownCode = (code >= static_cast<uint16_t>(EventCode::Boot) &&
                      code <= static_cast<uint16_t>(EventCode::SensorError)) ||
                     (code >= static_cast<uint16_t>(EventCode::I2cLockTimeout) &&
                      code <= static_cast<uint16_t>(EventCode::I2cCommunicationError)) ||
                     (code >= static_cast<uint16_t>(EventCode::Scd41Stale) &&
                     code <= static_cast<uint16_t>(EventCode::Scd41FactoryResetFailed)) ||
                     (code >= static_cast<uint16_t>(EventCode::FramRecordCorrupt) &&
                      code <= static_cast<uint16_t>(EventCode::FramCheckpointFailed)) ||
                     (code >= static_cast<uint16_t>(EventCode::PressureFieldUpdated) &&
                      code <= static_cast<uint16_t>(EventCode::Bmp581CalibrationFailed)) ||
                     (code >= static_cast<uint16_t>(EventCode::PpgSessionStarted) &&
                      code <= static_cast<uint16_t>(EventCode::PpgStorageFailed));
    if (!knownCode) return false;

    const uint8_t* payload = reinterpret_cast<const uint8_t*>(&record.uptimeMs);
    return calculateCrc16(payload, record.header.length) == record.header.crc;
}

void StorageManager::loadEventJournal() {
    eventWriteIndex_ = 0;
    eventCount_ = 0;
    nextEventSequence_ = 1;

    uint32_t newestSequence = 0;
    uint16_t newestIndex = 0;
    for (uint16_t i = 0; i < MAX_EVENT_RECORDS; ++i) {
        EventRecord record;
        if (!readEventSlot(i, record)) continue;

        eventCount_++;
        if (record.header.sequence > newestSequence) {
            newestSequence = record.header.sequence;
            newestIndex = i;
        }
    }

    if (newestSequence > 0) {
        eventWriteIndex_ = (newestIndex + 1) % MAX_EVENT_RECORDS;
        nextEventSequence_ = newestSequence + 1;
        if (nextEventSequence_ == 0) nextEventSequence_ = 1;
    }

    services::Logger::info("StorageMgr", "Event journal loaded. events=%u next_seq=%lu",
        eventCount_, nextEventSequence_);
}

bool StorageManager::appendEvent(EventCode code, int32_t detail, uint32_t uptimeMs) {
    if (!framAvailable_) return false;

    lock();

    EventRecord record;
    memset(&record, 0, sizeof(record));
    record.header.sequence = nextEventSequence_++;
    if (nextEventSequence_ == 0) nextEventSequence_ = 1;
    record.header.length = EVENT_PAYLOAD_SIZE;
    record.header.committed = 0;
    record.uptimeMs = uptimeMs;
    record.eventCode = static_cast<uint16_t>(code);
    record.detail = detail;
    record.header.crc = calculateCrc16(
        reinterpret_cast<const uint8_t*>(&record.uptimeMs), record.header.length);

    uint16_t addr = ADDR_EVENT_JOURNAL + (eventWriteIndex_ * EVENT_SLOT_SIZE);
    bool ok = fram_.write(addr, reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    if (ok) {
        ok = fram_.writeByte(addr + offsetof(EventRecord, header.committed), 1);
    }

    if (ok) {
        eventWriteIndex_ = (eventWriteIndex_ + 1) % MAX_EVENT_RECORDS;
        if (eventCount_ < MAX_EVENT_RECORDS) eventCount_++;
    }

    unlock();
    return ok;
}

size_t StorageManager::readRecentEvents(EventRecord* out, size_t maxCount) {
    if (!framAvailable_ || out == nullptr || maxCount == 0) return 0;

    lock();

    // Keep the newest maxCount records sorted by sequence. Scanning the small
    // fixed journal also tolerates a partially written slot after power loss.
    size_t count = 0;
    for (uint16_t i = 0; i < MAX_EVENT_RECORDS; ++i) {
        EventRecord record;
        if (!readEventSlot(i, record)) continue;

        size_t insertAt = count;
        while (insertAt > 0 && out[insertAt - 1].header.sequence > record.header.sequence) {
            insertAt--;
        }

        if (count < maxCount) {
            for (size_t j = count; j > insertAt; --j) out[j] = out[j - 1];
            out[insertAt] = record;
            count++;
        } else if (insertAt > 0) {
            for (size_t j = 0; j + 1 < insertAt; ++j) out[j] = out[j + 1];
            out[insertAt - 1] = record;
        }
    }

    unlock();
    return count;
}

bool StorageManager::appendRecord(const core::SensorSnapshot& snapshot, uint32_t uptimeMs) {
    lock();
    if (!framAvailable_) {
        unlock();
        return false;
    }

    // Record time is captured at append, independently of the age of the
    // latest GNSS position fix. UTC/source/discipline/PPS age come from one
    // coherent Clock snapshot.
    const core::TimeSnapshot time = hal::Clock::snapshot();

    if (superblock_.formatVersion == LEGACY_FRAM_FORMAT_VERSION &&
        FramWal<FramStorage>::pendingCount(superblock_) == 0) {
        const FramWalStatus migration =
            wal_.migrateEmptyToCurrent(superblock_, walStats_);
        if (migration != FramWalStatus::Ready) {
            framAvailable_ = false;
            framReadOnly_ = true;
            unlock();
            services::Logger::error("StorageMgr",
                "Failed to migrate empty v5 WAL to v6; preserving FRAM read-only");
            return false;
        }
        // A v6 record must never be appended to a CSV v6 file. Force the next
        // flush to create/select a schema-v7 target.
        currentFilename_ = "";
        currentDateString_ = "";
        sdAvailable_ = false;
        lastSdInitAttempt_ = 0;
        services::Logger::info("StorageMgr",
            "Migrated empty FRAM WAL from v5 to v6 without erasing ring slots");
    }

    FramWalStatus status = FramWalStatus::Incompatible;
    if (superblock_.formatVersion == LEGACY_FRAM_FORMAT_VERSION) {
        SensorRecordV5 rec {};
        rec.uptimeMs = uptimeMs;
        rec.sampleMonotonicUs = time.monotonicUs;
        rec.utcEpochMs = time.utcValid ? time.utcEpochUs / 1000LL : 0;
        rec.ppsAgeMs = time.ppsAgeMs;
        rec.timeSource = static_cast<uint8_t>(time.source);
        const uint8_t scd41State =
            static_cast<uint8_t>(snapshot.environment.scd41State) & 0x7u;
        rec.validFlags |= static_cast<uint32_t>(scd41State) << SCD41_STATE_SHIFT;
        rec.co2AgeSeconds = codec::ageSeconds(snapshot.environment.co2AgeMs);

        if (snapshot.environment.temperatureValid) {
            rec.temperatureC = snapshot.environment.temperatureC;
            rec.validFlags |= VALID_TEMP;
        }
        if (snapshot.environment.humidityValid) {
            rec.humidityRh = snapshot.environment.humidityRh;
            rec.validFlags |= VALID_HUMIDITY;
        }
        if (snapshot.environment.pressureValid) {
            rec.pressureHpa = snapshot.environment.pressureHpa;
            rec.validFlags |= VALID_PRESSURE;
        }
        if (snapshot.environment.altitudeValid) {
            rec.altitudeM = snapshot.environment.altitudeM;
            rec.validFlags |= VALID_ALTITUDE;
        }
        if (snapshot.environment.co2Valid && snapshot.environment.co2Ppm > 0) {
            rec.co2Ppm = snapshot.environment.co2Ppm;
            rec.validFlags |= VALID_CO2;
        }
        if (snapshot.environment.sgp41Valid) {
            rec.vocIndex = snapshot.environment.vocIndex;
            rec.noxIndex = snapshot.environment.noxIndex;
            rec.validFlags |= VALID_VOC | VALID_NOX;
        }
        if (snapshot.ppg.state == core::PpgState::Measuring &&
            snapshot.ppg.calculatedValid && !snapshot.ppg.signalPoor) {
            rec.heartRateBpm = snapshot.ppg.heartRateBpm;
            rec.spo2Percent = snapshot.ppg.spo2Percent;
            rec.validFlags |= VALID_HR | VALID_SPO2;
        }
        if (time.utcValid) rec.gnssValidFlags |= GNSS_VALID_UTC;
        if (time.disciplined) rec.gnssValidFlags |= GNSS_TIME_DISCIPLINED;
        if (time.ppsAgeMs <= 2500u) rec.gnssValidFlags |= GNSS_PPS_RECENT;
        if (snapshot.gnss.fixValid) {
            rec.gnssLatitudeE7 = static_cast<int32_t>(snapshot.gnss.latitudeDeg * 1e7);
            rec.gnssLongitudeE7 = static_cast<int32_t>(snapshot.gnss.longitudeDeg * 1e7);
            rec.gnssValidFlags |= GNSS_VALID_FIX;
        }
        if (snapshot.gnss.altitudeValid) {
            rec.gnssAltitudeMslM = snapshot.gnss.altitudeMslM;
            rec.gnssValidFlags |= GNSS_VALID_ALTITUDE;
        }
        if (snapshot.gnss.speedValid) {
            rec.gnssSpeedMps = snapshot.gnss.speedMps;
            rec.gnssValidFlags |= GNSS_VALID_SPEED;
        }
        if (snapshot.gnss.courseValid) {
            rec.gnssCourseDeg = snapshot.gnss.courseDeg;
            rec.gnssValidFlags |= GNSS_VALID_COURSE;
        }
        if (snapshot.gnss.hdopValid) {
            rec.gnssHdop = snapshot.gnss.hdop;
            rec.gnssValidFlags |= GNSS_VALID_HDOP;
        }
        rec.gnssSatellites = snapshot.gnss.satellites;
        rec.gnssAgeMs = snapshot.gnss.ageMs;
        if (snapshot.bme690.tphValid) {
            rec.bme690TemperatureC = snapshot.bme690.temperatureC;
            rec.bme690HumidityRh = snapshot.bme690.humidityRh;
            rec.bme690PressureHpa = snapshot.bme690.pressureHpa;
            rec.validFlags |= VALID_BME690_TPH;
        }
        if (snapshot.bme690.gasValid) {
            rec.bme690GasResistanceOhm = snapshot.bme690.gasResistanceOhm;
            rec.validFlags |= VALID_BME690_GAS;
        }
        rec.bme690GasIndex = snapshot.bme690.gasIndex;
        rec.bme690Status = snapshot.bme690.status;
        status = wal_.appendLegacy(rec, superblock_, walStats_);
    } else if (superblock_.formatVersion == FRAM_FORMAT_VERSION) {
        SensorRecordV6 rec {};
        rec.uptimeMs = uptimeMs;
        rec.sampleMonotonicUs = time.monotonicUs;
        rec.utcEpochMs = time.utcValid ? time.utcEpochUs / 1000LL : 0;
        rec.co2AgeSeconds = codec::ageSeconds(snapshot.environment.co2AgeMs);
        rec.sgp41AgeSeconds = codec::ageSeconds(snapshot.telemetry.sgp41.ageMs);
        rec.sht45AgeSeconds = codec::ageSeconds(snapshot.telemetry.sht45.ageMs);
        rec.bmp581AgeSeconds = codec::ageSeconds(snapshot.telemetry.bmp581.ageMs);
        rec.bme690AgeSeconds = codec::ageSeconds(snapshot.telemetry.bme690.ageMs);
        rec.seaLevelPressureAgeSeconds = codec::ageSeconds(
            snapshot.telemetry.altitude.seaLevelPressureAgeMs);
        rec.ppsAgeMs = codec::milliseconds16(time.ppsAgeMs);
        rec.sourceBits = codec::packSources(
            time.source, snapshot.telemetry.altitude.pressureState,
            snapshot.telemetry.altitude.pressureSource);
        rec.scd41Health = codec::packHealth(
            snapshot.telemetry.scd41.state, snapshot.telemetry.scd41.error,
            snapshot.telemetry.scd41.consecutiveErrors);
        rec.sgp41Health = codec::packHealth(
            snapshot.telemetry.sgp41.state, snapshot.telemetry.sgp41.error,
            snapshot.telemetry.sgp41.consecutiveErrors);
        rec.sht45Health = codec::packHealth(
            snapshot.telemetry.sht45.state, snapshot.telemetry.sht45.error,
            snapshot.telemetry.sht45.consecutiveErrors);
        rec.bmp581Health = codec::packHealth(
            snapshot.telemetry.bmp581.state, snapshot.telemetry.bmp581.error,
            snapshot.telemetry.bmp581.consecutiveErrors);
        rec.bme690Health = codec::packHealth(
            snapshot.telemetry.bme690.state, snapshot.telemetry.bme690.error,
            snapshot.telemetry.bme690.consecutiveErrors);
        rec.scd41RawError = snapshot.telemetry.scd41RawError;

        if (snapshot.environment.temperatureValid) {
            rec.temperatureCentiC = codec::signedFixed(
                snapshot.environment.temperatureC, 100.0f);
            rec.validFlags |= V6_VALID_TEMP;
        }
        if (snapshot.environment.humidityValid) {
            rec.humidityCentiRh = codec::unsignedFixed(
                snapshot.environment.humidityRh, 100.0f);
            rec.validFlags |= V6_VALID_HUMIDITY;
        }
        if (snapshot.environment.pressureValid) {
            rec.pressureDeciHpa = codec::unsignedFixed(
                snapshot.environment.pressureHpa, 10.0f);
            rec.validFlags |= V6_VALID_PRESSURE;
        }
        if (snapshot.environment.co2Valid && snapshot.environment.co2Ppm > 0) {
            rec.co2Ppm = snapshot.environment.co2Ppm;
            rec.validFlags |= V6_VALID_CO2;
        }
        if (snapshot.environment.sgp41Valid) {
            rec.vocIndex = codec::signedFixed(
                static_cast<float>(snapshot.environment.vocIndex), 1.0f);
            rec.noxIndex = codec::signedFixed(
                static_cast<float>(snapshot.environment.noxIndex), 1.0f);
            rec.validFlags |= V6_VALID_VOC | V6_VALID_NOX | V6_VALID_SGP41_RAW;
            rec.srawVoc = snapshot.telemetry.sgp41Raw.srawVoc;
            rec.srawNox = snapshot.telemetry.sgp41Raw.srawNox;
            rec.sgp41CompensationRhTicks =
                snapshot.telemetry.sgp41Raw.compensationRhTicks;
            rec.sgp41CompensationTemperatureTicks =
                snapshot.telemetry.sgp41Raw.compensationTemperatureTicks;
        }
        if (snapshot.environment.altitudeValid) {
            rec.displayAltitudeDeciM = codec::signedFixed(
                snapshot.telemetry.altitude.displayAltitudeM, 10.0f);
            rec.validFlags |= V6_VALID_DISPLAY_ALTITUDE;
        }
        if (snapshot.environment.pressureValid &&
            std::isfinite(snapshot.telemetry.altitude.rawAltitudeM)) {
            rec.rawAltitudeDeciM = codec::signedFixed(
                snapshot.telemetry.altitude.rawAltitudeM, 10.0f);
            rec.validFlags |= V6_VALID_RAW_ALTITUDE;
        }
        const core::PressureFieldState pressureState =
            snapshot.telemetry.altitude.pressureState;
        if ((pressureState == core::PressureFieldState::Valid ||
             pressureState == core::PressureFieldState::LastKnown ||
             pressureState == core::PressureFieldState::StaticFallback) &&
            std::isfinite(snapshot.telemetry.altitude.seaLevelPressureHpa)) {
            rec.seaLevelPressureDeciHpa = codec::unsignedFixed(
                snapshot.telemetry.altitude.seaLevelPressureHpa, 10.0f);
            rec.validFlags |= V6_VALID_SEA_LEVEL_PRESSURE;
        }
        if (superblock_.hasValidBmp581Calibration) {
            rec.pressureOffsetCentiHpa = codec::signedFixed(
                snapshot.telemetry.altitude.pressureOffsetHpa, 100.0f);
            rec.validFlags |= V6_VALID_PRESSURE_OFFSET;
        }
        if (snapshot.ppg.state == core::PpgState::Measuring &&
            snapshot.ppg.calculatedValid && !snapshot.ppg.signalPoor) {
            rec.heartRateDeciBpm = codec::unsignedFixed(
                snapshot.ppg.heartRateBpm, 10.0f);
            rec.spo2CentiPercent = codec::unsignedFixed(
                snapshot.ppg.spo2Percent, 100.0f);
            rec.validFlags |= V6_VALID_HR | V6_VALID_SPO2 |
                              V6_VALID_PPG_QUALITY;
        }

        if (time.utcValid) rec.gnssValidFlags |= GNSS_VALID_UTC;
        if (time.disciplined) rec.gnssValidFlags |= GNSS_TIME_DISCIPLINED;
        if (time.ppsAgeMs <= 2500u) rec.gnssValidFlags |= GNSS_PPS_RECENT;
        if (snapshot.gnss.fixValid) {
            rec.gnssLatitudeE7 = static_cast<int32_t>(snapshot.gnss.latitudeDeg * 1e7);
            rec.gnssLongitudeE7 = static_cast<int32_t>(snapshot.gnss.longitudeDeg * 1e7);
            rec.gnssValidFlags |= GNSS_VALID_FIX;
        }
        if (snapshot.gnss.altitudeValid) {
            rec.gnssAltitudeDeciM = codec::signedFixed(
                snapshot.gnss.altitudeMslM, 10.0f);
            rec.gnssValidFlags |= GNSS_VALID_ALTITUDE;
        }
        if (snapshot.gnss.speedValid) {
            rec.gnssSpeedCentiMps = codec::unsignedFixed(
                snapshot.gnss.speedMps, 100.0f);
            rec.gnssValidFlags |= GNSS_VALID_SPEED;
        }
        if (snapshot.gnss.courseValid) {
            rec.gnssCourseDeciDeg = codec::unsignedFixed(
                snapshot.gnss.courseDeg, 10.0f);
            rec.gnssValidFlags |= GNSS_VALID_COURSE;
        }
        if (snapshot.gnss.hdopValid) {
            rec.gnssHdopCenti = codec::unsignedFixed(
                snapshot.gnss.hdop, 100.0f);
            rec.gnssValidFlags |= GNSS_VALID_HDOP;
        }
        rec.gnssAgeSeconds = codec::ageSeconds(snapshot.gnss.ageMs);
        rec.gnssSatellites = static_cast<uint8_t>(
            snapshot.gnss.satellites > UINT8_MAX
                ? UINT8_MAX : snapshot.gnss.satellites);

        if (snapshot.bme690.tphValid) {
            rec.bme690TemperatureCentiC = codec::signedFixed(
                snapshot.bme690.temperatureC, 100.0f);
            rec.bme690HumidityCentiRh = codec::unsignedFixed(
                snapshot.bme690.humidityRh, 100.0f);
            rec.bme690PressureDeciHpa = codec::unsignedFixed(
                snapshot.bme690.pressureHpa, 10.0f);
            rec.validFlags |= V6_VALID_BME690_TPH;
        }
        if (snapshot.bme690.gasValid) {
            rec.bme690GasResistanceOhm = codec::unsignedWhole(
                snapshot.bme690.gasResistanceOhm);
            rec.validFlags |= V6_VALID_BME690_GAS;
        }
        rec.bme690GasIndex = snapshot.bme690.gasIndex;
        rec.bme690Status = snapshot.bme690.status;
        rec.i2cLockTimeouts = codec::count16(
            snapshot.telemetry.i2c.lockTimeouts);
        rec.i2cCommunicationErrors = codec::count16(
            snapshot.telemetry.i2c.communicationErrors);
        rec.droppedRecords = codec::count16(walStats_.droppedRecords);
        rec.formatTag = codec::SENSOR_RECORD_V6_TAG;
        status = wal_.append(rec, superblock_, walStats_);
    }

    const uint32_t droppedRecords = walStats_.droppedRecords;
    const bool reportFull = status == FramWalStatus::Full &&
        (lastRingFullEventMs_ == 0 || uptimeMs - lastRingFullEventMs_ >= 60000u);
    if (reportFull) lastRingFullEventMs_ = uptimeMs;
    if (status != FramWalStatus::Ready && status != FramWalStatus::Full) {
        // A failed checkpoint may already be durable. Stop all later writes so
        // this slot cannot be reused until reboot reconciliation chooses the
        // newest valid checkpoint or adopts the committed orphan.
        framAvailable_ = false;
        framReadOnly_ = true;
    }
    unlock();

    if (status == FramWalStatus::Full) {
        services::Logger::warn("StorageMgr",
            "FRAM ring full; preserving %u pending records (drop count=%lu)",
            MAX_RECORDS - 1, static_cast<unsigned long>(droppedRecords));
        if (reportFull) appendEvent(EventCode::RingBufferFull,
            static_cast<int32_t>(droppedRecords), uptimeMs);
        return false;
    }
    if (status != FramWalStatus::Ready) {
        services::Logger::error("StorageMgr", "FRAM WAL append/checkpoint failed (%u)",
            static_cast<unsigned>(status));
        return false;
    }
    return true;
}

bool StorageManager::initSdCard() {
    if (sdAvailable_) return true; // すでに有効

    // 再試行間隔 (5秒)
    if (millis() - lastSdInitAttempt_ < 5000 && lastSdInitAttempt_ != 0) {
        return false;
    }
    lastSdInitAttempt_ = millis();

    // 以前のマウント状態をクリアするため、一度end()を呼ぶ
    SD.end();

    // CSピンを明示的にOUTPUT/HIGHに設定して安定させる
    pinMode(hal::pins::SD_CS, OUTPUT);
    digitalWrite(hal::pins::SD_CS, HIGH);
    delay(10);

    SPI.begin(SCK, MISO, MOSI, -1); // Hardware CSを無効化し、SDライブラリにCS制御を委ねる
    if (!SD.begin(hal::pins::SD_CS, SPI, 4000000)) {
        services::Logger::error("StorageMgr", "SD Card Mount Failed.");
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        services::Logger::error("StorageMgr", "No SD card attached");
        return false;
    }

    // すでにファイル名が決まっている（つまり再挿入された）場合は、続きに追記する
    if (currentFilename_.length() == 0) {
        sdAvailable_ = createNewSdFile();
    } else {
        services::Logger::info("StorageMgr", "SD Card remounted. Continuing with %s", currentFilename_.c_str());
        sdAvailable_ = true;
    }
    
    return sdAvailable_;
}

bool StorageManager::createNewSdFile(const String& targetDate) {
    const uint16_t schemaVersion = activeCsvSchemaVersion();
    if (hal::Clock::isTimeSet()) {
        currentDateString_ = (targetDate.length() > 0) ? targetDate : hal::Clock::getFormattedDate();
        currentFilename_ = "/log_" + currentDateString_ + "_v" + String(schemaVersion) + ".csv";
    } else {
        currentDateString_ = "";
        for (int i = 0; i < 1000; i++) {
            char filename[32];
            snprintf(filename, sizeof(filename), "/log_boot_%03d_v%u.csv", i, schemaVersion);
            if (!SD.exists(filename)) {
                currentFilename_ = filename;
                break;
            }
        }
    }

    bool exists = SD.exists(currentFilename_);
    File file = SD.open(currentFilename_.c_str(), FILE_APPEND);
    if (!file) {
        services::Logger::error("StorageMgr", "Failed to create/open file: %s", currentFilename_.c_str());
        return false;
    }
    
    if (!exists) {
        writeCsvHeader(file, schemaVersion);
    }
    file.close();
    services::Logger::info("StorageMgr", "Target SD log file: %s", currentFilename_.c_str());
    return true;
}

uint16_t StorageManager::activeCsvSchemaVersion() const {
    return superblock_.formatVersion == LEGACY_FRAM_FORMAT_VERSION
        ? LEGACY_CSV_SCHEMA_VERSION : CSV_SCHEMA_VERSION;
}

void StorageManager::writeCsvHeader(File& file, uint16_t schemaVersion) {
    if (schemaVersion == CSV_SCHEMA_VERSION) {
        file.println(CSV_V7_HEADER);
        return;
    }
    file.println("Sequence,UptimeMs,SampleMonotonicUs,TimestampUtc,TimeSource,CO2_ppm,Temp_C,RH_pct,Pressure_hPa,VOC_Index,NOx_Index,HR_bpm,SpO2_pct,BMP_Altitude_m,GNSS_Lat_deg,GNSS_Lon_deg,GNSS_AltMSL_m,GNSS_Speed_mps,GNSS_Course_deg,GNSS_Satellites,GNSS_HDOP,GNSS_FixValid,GNSS_TimeValid,GNSS_AgeMs,PPS_AgeMs,GNSS_TimeDisciplined,ValidFlags,BME690_Temp_C,BME690_RH_pct,BME690_Pressure_hPa,BME690_GasResistance_Ohm,BME690_GasValid,BME690_HeaterStable,BME690_GasIndex,BME690_StatusHex,CO2_Valid,CO2_AgeMs,SCD41_State");
}

void StorageManager::formatCsvLineV5(char* buffer, size_t size, const SensorRecordV5& rec) {
    float temp = (rec.validFlags & VALID_TEMP) ? rec.temperatureC : NAN;
    float rh = (rec.validFlags & VALID_HUMIDITY) ? rec.humidityRh : NAN;
    float press = (rec.validFlags & VALID_PRESSURE) ? rec.pressureHpa : NAN;
    float co2 = (rec.validFlags & VALID_CO2) ? (float)rec.co2Ppm : NAN;
    float voc = (rec.validFlags & VALID_VOC) ? rec.vocIndex : NAN;
    float nox = (rec.validFlags & VALID_NOX) ? rec.noxIndex : NAN;
    float hr = (rec.validFlags & VALID_HR) ? rec.heartRateBpm : NAN;
    float spo2 = (rec.validFlags & VALID_SPO2) ? rec.spo2Percent : NAN;
    float alt = (rec.validFlags & VALID_ALTITUDE) ? rec.altitudeM : NAN;
    
    // GNSS Fields
    double lat = (rec.gnssValidFlags & GNSS_VALID_FIX) ? (rec.gnssLatitudeE7 / 1e7) : NAN;
    double lon = (rec.gnssValidFlags & GNSS_VALID_FIX) ? (rec.gnssLongitudeE7 / 1e7) : NAN;
    float gnssAlt = (rec.gnssValidFlags & GNSS_VALID_ALTITUDE) ? rec.gnssAltitudeMslM : NAN;
    float speed = (rec.gnssValidFlags & GNSS_VALID_SPEED) ? rec.gnssSpeedMps : NAN;
    float course = (rec.gnssValidFlags & GNSS_VALID_COURSE) ? rec.gnssCourseDeg : NAN;
    float hdop = (rec.gnssValidFlags & GNSS_VALID_HDOP) ? rec.gnssHdop : NAN;
    
    // BME690 Fields
    float bmeTemp = (rec.validFlags & VALID_BME690_TPH) ? rec.bme690TemperatureC : NAN;
    float bmeRh = (rec.validFlags & VALID_BME690_TPH) ? rec.bme690HumidityRh : NAN;
    float bmePress = (rec.validFlags & VALID_BME690_TPH) ? rec.bme690PressureHpa : NAN;
    float bmeGas = (rec.validFlags & VALID_BME690_GAS) ? rec.bme690GasResistanceOhm : NAN;
    uint8_t bmeGasValid = (rec.validFlags & VALID_BME690_GAS) ? 1 : 0;
    uint8_t bmeHeaterStable = (rec.bme690Status & 0x10) ? 1 : 0; // HEAT_STAB_MSK is 0x10

    int32_t co2AgeMs = rec.co2AgeSeconds == UINT16_MAX
        ? -1
        : static_cast<int32_t>(rec.co2AgeSeconds) * 1000;
    const char* scd41State = "UNKNOWN";
    switch ((rec.validFlags & SCD41_STATE_MASK) >> SCD41_STATE_SHIFT) {
        case 1: scd41State = "INITIALIZING"; break;
        case 2: scd41State = "READY"; break;
        case 3: scd41State = "DEGRADED"; break;
        case 4: scd41State = "WARNING"; break;
        case 5: scd41State = "OFFLINE"; break;
        case 6: scd41State = "RETRY_WAIT"; break;
        case 7: scd41State = "ERROR"; break;
        default: break;
    }

    const char* tsStr = "UNSET";
    switch (static_cast<core::TimeSource>(rec.timeSource)) {
        case core::TimeSource::Manual: tsStr = "MANUAL"; break;
        case core::TimeSource::Ntp: tsStr = "NTP"; break;
        case core::TimeSource::Gnss: tsStr = "GNSS"; break;
        case core::TimeSource::Holdover: tsStr = "HOLDOVER"; break;
        default: break;
    }

    char timeStr[32] = "";
    if (rec.gnssValidFlags & GNSS_VALID_UTC) {
        time_t recordEpoch = rec.utcEpochMs / 1000;
        uint16_t ms = rec.utcEpochMs % 1000;
        struct tm timeinfo;
        gmtime_r(&recordEpoch, &timeinfo);
        
        snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, ms);
    } else {
        // Fallback or leave empty
        snprintf(timeStr, sizeof(timeStr), "INVALID");
    }

    // Since the buffer might be tight for all these formats, snprintf handles truncation safely
    int charsWritten = snprintf(buffer, size, "%lu,%lu,%lld,%s,%s,%.1f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.7f,%.7f,%.1f,%.1f,%.1f,%u,%.1f,%d,%d,%lu,%lu,%d,0x%08lX,%.2f,%.2f,%.2f,%.0f,%d,%d,%d,0x%02X,%d,%ld,%s",
             rec.sequence, rec.uptimeMs, rec.sampleMonotonicUs, timeStr, tsStr, 
             co2, temp, rh, press, voc, nox, hr, spo2, alt, 
             lat, lon, gnssAlt, speed, course, rec.gnssSatellites, hdop, 
             (rec.gnssValidFlags & GNSS_VALID_FIX) ? 1 : 0, 
             (rec.gnssValidFlags & GNSS_VALID_UTC) ? 1 : 0, 
             rec.gnssAgeMs, rec.ppsAgeMs, 
             (rec.gnssValidFlags & GNSS_TIME_DISCIPLINED) ? 1 : 0, 
             static_cast<unsigned long>(rec.validFlags),
             bmeTemp, bmeRh, bmePress, bmeGas, bmeGasValid, bmeHeaterStable,
             rec.bme690GasIndex, rec.bme690Status,
             (rec.validFlags & VALID_CO2) ? 1 : 0,
             static_cast<long>(co2AgeMs), scd41State);
             
    if (charsWritten < 0 || (size_t)charsWritten >= size) {
        services::Logger::warn("StorageMgr", "CSV line truncated. Need %d bytes, got %zu bytes", charsWritten, size);
        buffer[0] = '\0';
    }
}

bool StorageManager::appendAndVerifyCsvLine(const char* line, size_t lineLength) {
    if (line == nullptr || lineLength == 0 ||
        lineLength >= MAX_CSV_LINE_LENGTH) return false;

    uint32_t originalSize = 0;
    uint32_t expectedStart = 0;
    size_t prefixLength = 0;

    File inspect = SD.open(currentFilename_.c_str(), FILE_READ);
    if (!inspect) return false;
    originalSize = inspect.size();

    char tail[MAX_CSV_LINE_LENGTH + 1] {};
    const uint32_t readStart = originalSize > MAX_CSV_LINE_LENGTH
        ? originalSize - MAX_CSV_LINE_LENGTH : 0;
    const size_t requested = originalSize - readStart;
    if (!inspect.seek(readStart) || inspect.read(
            reinterpret_cast<uint8_t*>(tail), requested) != requested) {
        inspect.close();
        return false;
    }
    inspect.close();

    if (requested > 0 && tail[requested - 1] == '\n') {
        size_t lastStart = requested - 1;
        while (lastStart > 0 && tail[lastStart - 1] != '\n') --lastStart;
        const size_t lastLength = requested - 1 - lastStart;
        if (lastLength == lineLength && memcmp(tail + lastStart, line, lineLength) == 0) {
            return true; // SD append completed before a reset; consume only.
        }
        expectedStart = originalSize;
    } else {
        size_t fragmentStart = requested;
        while (fragmentStart > 0 && tail[fragmentStart - 1] != '\n') --fragmentStart;
        prefixLength = requested - fragmentStart;
        if (prefixLength > lineLength ||
            memcmp(tail + fragmentStart, line, prefixLength) != 0) {
            services::Logger::error("StorageMgr",
                "CSV has an unexpected unterminated tail; preserving FRAM record");
            return false;
        }
        expectedStart = originalSize - prefixLength;
    }

    File output = SD.open(currentFilename_.c_str(), FILE_APPEND);
    if (!output) return false;
    const size_t remainder = lineLength - prefixLength;
    const size_t wroteData = output.write(
        reinterpret_cast<const uint8_t*>(line + prefixLength), remainder);
    const size_t wroteNewline = wroteData == remainder
        ? output.write(reinterpret_cast<const uint8_t*>("\n"), 1) : 0;
    output.flush();
    output.close();
    if (wroteData != remainder || wroteNewline != 1) return false;

    File verify = SD.open(currentFilename_.c_str(), FILE_READ);
    if (!verify || verify.size() != expectedStart + lineLength + 1 ||
        !verify.seek(expectedStart)) {
        if (verify) verify.close();
        return false;
    }
    char actual[MAX_CSV_LINE_LENGTH + 1] {};
    const size_t expectedBytes = lineLength + 1;
    const size_t actualBytes = verify.read(reinterpret_cast<uint8_t*>(actual), expectedBytes);
    verify.close();
    return actualBytes == expectedBytes && actual[lineLength] == '\n' &&
           memcmp(actual, line, lineLength) == 0;
}

bool StorageManager::appendAndVerifyQuarantine(const FramQuarantineRecord& record) {
    static constexpr const char* path = "/fram_quarantine_v1.bin";
    uint32_t originalSize = 0;

    File inspect = SD.open(path, FILE_READ);
    if (inspect) {
        originalSize = inspect.size();
        if (originalSize >= sizeof(FramQuarantineRecord) &&
            inspect.seek(originalSize - sizeof(FramQuarantineRecord))) {
            FramQuarantineRecord previous {};
            const size_t readBytes = inspect.read(
                reinterpret_cast<uint8_t*>(&previous), sizeof(previous));
            if (readBytes == sizeof(previous) &&
                previous.magic == FRAM_QUARANTINE_MAGIC &&
                previous.version == FRAM_QUARANTINE_VERSION &&
                previous.rawLength == RECORD_SLOT_SIZE &&
                previous.rawCrc16 == calculateCrc16(previous.raw, sizeof(previous.raw)) &&
                previous.recordCrc16 == calculateCrc16(
                    reinterpret_cast<const uint8_t*>(&previous),
                    offsetof(FramQuarantineRecord, recordCrc16)) &&
                previous.slotIndex == record.slotIndex &&
                memcmp(previous.raw, record.raw, sizeof(record.raw)) == 0) {
                inspect.close();
                return true; // SD commit survived reset; advance WAL only.
            }
        }
        inspect.close();
    }

    File output = SD.open(path, FILE_APPEND);
    if (!output) return false;
    const size_t written = output.write(
        reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    output.flush();
    output.close();
    if (written != sizeof(record)) return false;

    File verify = SD.open(path, FILE_READ);
    if (!verify || verify.size() != originalSize + sizeof(record) ||
        !verify.seek(originalSize)) {
        if (verify) verify.close();
        return false;
    }
    FramQuarantineRecord actual {};
    const size_t readBytes = verify.read(
        reinterpret_cast<uint8_t*>(&actual), sizeof(actual));
    verify.close();
    return readBytes == sizeof(actual) &&
           memcmp(&actual, &record, sizeof(record)) == 0;
}

bool StorageManager::quarantineCorruptTail(uint16_t slotIndex, uint32_t uptimeMs) {
    uint8_t first[RECORD_SLOT_SIZE] {};
    uint8_t second[RECORD_SLOT_SIZE] {};
    if (wal_.readRawSlot(slotIndex, first, sizeof(first)) != FramWalStatus::Ready ||
        wal_.readRawSlot(slotIndex, second, sizeof(second)) != FramWalStatus::Ready ||
        memcmp(first, second, sizeof(first)) != 0) {
        services::Logger::error("StorageMgr",
            "FRAM corrupt-tail reads are unstable; slot %u retained", slotIndex);
        return false;
    }

    // A transient I2C error may have caused the first failed peek. Never
    // quarantine a slot that has become structurally readable meanwhile.
    FramWalStatus retryStatus = FramWalStatus::Incompatible;
    if (superblock_.formatVersion == LEGACY_FRAM_FORMAT_VERSION) {
        PersistentRecordV5 retry {};
        retryStatus = wal_.peekLegacy(superblock_, retry);
    } else if (superblock_.formatVersion == FRAM_FORMAT_VERSION) {
        PersistentRecordV6 retry {};
        retryStatus = wal_.peek(superblock_, retry);
    }
    if (retryStatus == FramWalStatus::Ready) {
        services::Logger::warn("StorageMgr",
            "FRAM slot %u became readable; deferring to normal flush", slotIndex);
        return false;
    }
    if (retryStatus != FramWalStatus::Corrupt) {
        services::Logger::error("StorageMgr",
            "FRAM slot %u could not be classified reliably; retained", slotIndex);
        return false;
    }

    FramQuarantineRecord evidence {};
    evidence.magic = FRAM_QUARANTINE_MAGIC;
    evidence.version = FRAM_QUARANTINE_VERSION;
    evidence.slotIndex = slotIndex;
    evidence.captureBootCount = superblock_.bootCount;
    evidence.captureUptimeMs = uptimeMs;
    evidence.rawLength = RECORD_SLOT_SIZE;
    memcpy(evidence.raw, first, sizeof(evidence.raw));
    evidence.rawCrc16 = calculateCrc16(evidence.raw, sizeof(evidence.raw));
    evidence.recordCrc16 = calculateCrc16(
        reinterpret_cast<const uint8_t*>(&evidence),
        offsetof(FramQuarantineRecord, recordCrc16));

    if (!appendAndVerifyQuarantine(evidence)) {
        services::Logger::error("StorageMgr",
            "Failed to verify SD quarantine evidence; slot %u retained", slotIndex);
        return false;
    }
    if (wal_.quarantineTail(slotIndex, superblock_, walStats_) != FramWalStatus::Ready) {
        services::Logger::error("StorageMgr",
            "Quarantine is on SD but WAL checkpoint failed; entering read-only mode");
        framAvailable_ = false;
        framReadOnly_ = true;
        return false;
    }

    services::Logger::warn("StorageMgr",
        "Quarantined corrupt FRAM slot %u to %s", slotIndex,
        "/fram_quarantine_v1.bin");
    appendEvent(EventCode::FramRecordCorrupt, static_cast<int32_t>(slotIndex), uptimeMs);
    return true;
}

void StorageManager::flushPendingToSd() {
    lock();
    if (!framAvailable_) {
        unlock();
        return;
    }

    // Wi-Fiモード中はSDへの書き出しを停止し、FRAMにためておく
    // これによりWebサーバーからのファイルダウンロード時のSPIバス競合(WDTクラッシュなど)を完全に防ぐ
    if (wifiActive_) {
        unlock();
        return;
    }

    uint16_t pendingCount = getPendingCount();
    if (pendingCount == 0) {
        unlock();
        return;
    }

    // ローテーションチェック
    if (hal::Clock::isTimeSet() && !sdTransaction_.active()) {
        // 実際の現在日付で書き込み先切り替え判定 (0時0分0秒に切り替え)
        String today = hal::Clock::getFormattedDate();
        if (today != currentDateString_) {
            services::Logger::info("StorageMgr", "Log target switched: %s -> %s", 
                                   currentDateString_.c_str(), today.c_str());
            createNewSdFile(today);
        }

        // 翌日のファイル事前作成チェック (日付が変わる5分前以降)
        time_t nowEpoch = hal::Clock::getEpoch() + (9 * 3600); // JST
        time_t shiftedEpoch = nowEpoch + 300;
        
        struct tm timeinfoNow, timeinfoShifted;
        gmtime_r(&nowEpoch, &timeinfoNow);
        gmtime_r(&shiftedEpoch, &timeinfoShifted);

        if (timeinfoNow.tm_mday != timeinfoShifted.tm_mday) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d%02d%02d", 
                     timeinfoShifted.tm_year + 1900, timeinfoShifted.tm_mon + 1, timeinfoShifted.tm_mday);
            String tomorrow = String(buf);
            const uint16_t schemaVersion = activeCsvSchemaVersion();
            String tomorrowFilename = "/log_" + tomorrow + "_v" + String(schemaVersion) + ".csv";
            
            if (!SD.exists(tomorrowFilename)) {
                services::Logger::info("StorageMgr", "Pre-creating tomorrow's file: %s", tomorrowFilename.c_str());
                File file = SD.open(tomorrowFilename.c_str(), FILE_WRITE);
                if (file) {
                    writeCsvHeader(file, schemaVersion);
                    file.close();
                } else {
                    services::Logger::error("StorageMgr", "Failed to pre-create: %s", tomorrowFilename.c_str());
                }
            }
        }
    }

    if (!initSdCard()) {
        services::Logger::warn("StorageMgr", "SD offline. Buffering %u records in FRAM.", pendingCount);
        unlock();
        return;
    }

    uint16_t handledCount = 0;
    uint16_t flushedCount = 0;
    uint16_t quarantinedCount = 0;
    while (handledCount < pendingCount) {
        const bool resumedTransaction = sdTransaction_.active();
        char line[MAX_CSV_LINE_LENGTH] {};
        uint32_t sequence = 0;
        bool lineFormatted = false;
        FramWalStatus walStatus = FramWalStatus::Incompatible;

        if (superblock_.formatVersion == LEGACY_FRAM_FORMAT_VERSION) {
            PersistentRecordV5 rec {};
            walStatus = wal_.peekLegacy(superblock_, rec);
            if (walStatus == FramWalStatus::Corrupt) {
                // Require repeated structural failure before treating bytes as
                // corrupt. I/O failures are never converted into data loss.
                PersistentRecordV5 retry {};
                walStatus = wal_.peekLegacy(superblock_, retry);
                if (walStatus == FramWalStatus::Ready) rec = retry;
            }
            if (walStatus == FramWalStatus::Ready) {
                if (rec.header.length == LEGACY_SENSOR_RECORD_V5_SIZE) {
                    rec.data.co2AgeSeconds = UINT16_MAX;
                    rec.data.validFlags &= ~SCD41_STATE_MASK;
                }
                sequence = rec.header.sequence;
                formatCsvLineV5(line, sizeof(line), rec.data);
                lineFormatted = line[0] != '\0';
            }
        } else if (superblock_.formatVersion == FRAM_FORMAT_VERSION) {
            PersistentRecordV6 rec {};
            walStatus = wal_.peek(superblock_, rec);
            if (walStatus == FramWalStatus::Corrupt) {
                PersistentRecordV6 retry {};
                walStatus = wal_.peek(superblock_, retry);
                if (walStatus == FramWalStatus::Ready) rec = retry;
            }
            if (walStatus == FramWalStatus::Ready) {
                sequence = rec.header.sequence;
                lineFormatted = formatCsvLineV7(line, sizeof(line), rec.data);
            }
        }

        if (walStatus == FramWalStatus::Corrupt &&
            quarantineCorruptTail(superblock_.readIndex, millis())) {
            ++handledCount;
            ++quarantinedCount;
            continue;
        }
        if (walStatus != FramWalStatus::Ready) {
            services::Logger::error("StorageMgr",
                "FRAM tail is unreadable; no records consumed (slot=%u status=%u)",
                superblock_.readIndex, static_cast<unsigned>(walStatus));
            break;
        }
        if (!lineFormatted) {
            services::Logger::error("StorageMgr",
                "CSV formatting failed at sequence %lu; FRAM record retained",
                static_cast<unsigned long>(sequence));
            break;
        }
        const size_t lineLength = strlen(line);
        if (sdTransaction_.active()) {
            if (sdTransaction_.current().sequence != sequence ||
                sdTransaction_.current().lineLength != lineLength ||
                sdTransaction_.current().lineCrc16 != calculateCrc16(
                    reinterpret_cast<const uint8_t*>(line), lineLength)) {
                services::Logger::error("StorageMgr",
                    "SD transaction does not match FRAM tail; preserving both");
                framAvailable_ = false;
                framReadOnly_ = true;
                break;
            }
            currentFilename_ = sdTransaction_.current().filename;
        } else if (sdTransaction_.start(sequence, currentFilename_.c_str(),
                       reinterpret_cast<const uint8_t*>(line), lineLength) !=
                   FramWalStatus::Ready) {
            services::Logger::error("StorageMgr",
                "Failed to persist SD transaction for sequence %lu",
                static_cast<unsigned long>(sequence));
            framAvailable_ = false;
            framReadOnly_ = true;
            break;
        }

        if (lineLength == 0 || !appendAndVerifyCsvLine(line, lineLength)) {
            services::Logger::error("StorageMgr",
                "SD append verification failed at sequence %lu; FRAM record retained",
                static_cast<unsigned long>(sequence));
            sdAvailable_ = false;
            appendEvent(EventCode::SdWriteFailed,
                static_cast<int32_t>(sequence), millis());
            break;
        }

        walStatus = wal_.consume(sequence, superblock_, walStats_);
        if (walStatus != FramWalStatus::Ready) {
            services::Logger::error("StorageMgr",
                "SD is verified but FRAM consume checkpoint failed at sequence %lu",
                static_cast<unsigned long>(sequence));
            framAvailable_ = false;
            framReadOnly_ = true;
            break;
        }
        if (sdTransaction_.finish(sequence) != FramWalStatus::Ready) {
            services::Logger::error("StorageMgr",
                "FRAM consumed but SD transaction cleanup failed at sequence %lu",
                static_cast<unsigned long>(sequence));
            framAvailable_ = false;
            framReadOnly_ = true;
            break;
        }
        ++handledCount;
        ++flushedCount;
        if (resumedTransaction) break; // Rotate/select the normal target on the next flush.
    }

    if (flushedCount > 0) {
        services::Logger::info("StorageMgr", "Verified and consumed %u FRAM records", flushedCount);
    }
    if (quarantinedCount > 0) {
        services::Logger::warn("StorageMgr",
            "Preserved and skipped %u corrupt FRAM records", quarantinedCount);
    }
    unlock();
}

} // namespace storage
