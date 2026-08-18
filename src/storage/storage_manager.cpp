#include "storage/storage_manager.h"
#include "services/logger.h"
#include "hal/pins.h"
#include "hal/clock.h"
#include <Arduino.h>

namespace storage {

StorageManager::StorageManager() : sdAvailable_(false), framAvailable_(false), lastSdInitAttempt_(0) {
    mutex_ = xSemaphoreCreateMutex();
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

void StorageManager::lock() {
    if (mutex_) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
}

void StorageManager::unlock() {
    if (mutex_) {
        xSemaphoreGive(mutex_);
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
        if (!loadSuperblock()) {
            services::Logger::warn("StorageMgr", "Superblock invalid. Initializing FRAM...");
            initSuperblock();
            saveSuperblock();
        } else {
            superblock_.bootCount++;
            saveSuperblock();
            services::Logger::info("StorageMgr", "FRAM loaded. Boot count: %u, pending: %u", 
                superblock_.bootCount, getPendingCount());
        }
    }

    initSdCard();

    return framAvailable_ || sdAvailable_;
}

void StorageManager::initSuperblock() {
    superblock_.magic = FRAM_MAGIC;
    superblock_.formatVersion = FRAM_FORMAT_VERSION;
    superblock_.writeIndex = 0;
    superblock_.readIndex = 0;
    superblock_.nextSequence = 1;
    superblock_.bootCount = 1;
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
    if (!framAvailable_ || !superblock_.hasValidSgp41State) return false;
    voc0 = superblock_.sgp41VocState0;
    voc1 = superblock_.sgp41VocState1;
    return true;
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
    if (!framAvailable_) return 0;
    return superblock_.lastScd41CalibrationEpoch;
}

void StorageManager::setScd41LastCalibrationEpoch(uint32_t epoch) {
    if (!framAvailable_) return;
    
    lock();
    superblock_.lastScd41CalibrationEpoch = epoch;
    saveSuperblock();
    unlock();
}

bool StorageManager::getBmp581Calibration(float& offsetHpa, uint32_t& epoch, float& tempC, float& slpHpa) const {
    if (!framAvailable_ || !superblock_.hasValidBmp581Calibration) return false;
    offsetHpa = superblock_.bmp581PressureOffsetHpa;
    epoch = superblock_.bmp581CalibEpoch;
    tempC = superblock_.bmp581CalibTempC;
    slpHpa = superblock_.bmp581CalibSeaLevelHpa;
    return true;
}

void StorageManager::setBmp581Calibration(float offsetHpa, uint32_t epoch, float tempC, float slpHpa) {
    if (!framAvailable_) return;
    
    lock();
    superblock_.hasValidBmp581Calibration = true;
    superblock_.bmp581PressureOffsetHpa = offsetHpa;
    superblock_.bmp581CalibEpoch = epoch;
    superblock_.bmp581CalibTempC = tempC;
    superblock_.bmp581CalibSeaLevelHpa = slpHpa;
    saveSuperblock();
    unlock();
}

bool StorageManager::loadSuperblock() {
    uint8_t buffer[sizeof(FramSuperblock)];
    if (!fram_.read(ADDR_SUPERBLOCK, buffer, sizeof(buffer))) return false;

    FramSuperblock* sb = reinterpret_cast<FramSuperblock*>(buffer);
    if (sb->magic != FRAM_MAGIC) {
        return false;
    }
    
    if (sb->formatVersion == 3) {
        services::Logger::info("StorageMgr", "Detected v3 FRAM format. Checking for unflushed records...");
        uint16_t oldMaxRecords = RING_BUFFER_SIZE / 64;
        uint16_t pending = (sb->writeIndex >= sb->readIndex) ? 
                           (sb->writeIndex - sb->readIndex) : 
                           (oldMaxRecords - sb->readIndex + sb->writeIndex);
        if (pending > 0) {
            services::Logger::error("StorageMgr", "Found %u unflushed v3 records! Flush them before upgrading to v4.", pending);
            return false; // Prevent using FRAM until flushed
        }
        
        services::Logger::info("StorageMgr", "No pending v3 records. Migrating to v4 format.");
        sb->formatVersion = 4;
        sb->writeIndex = 0;
        sb->readIndex = 0;
        
        // Recompute CRC for the migrated superblock
        sb->crc16 = calculateCrc16(buffer, sizeof(FramSuperblock) - sizeof(uint16_t));
        
        superblock_ = *sb;
        saveSuperblock();
        return true;
    } else if (sb->formatVersion != FRAM_FORMAT_VERSION) {
        return false;
    }

    uint16_t crc = calculateCrc16(buffer, sizeof(FramSuperblock) - sizeof(uint16_t));
    if (crc != sb->crc16) {
        services::Logger::warn("StorageMgr", "Superblock CRC mismatch");
        return false;
    }

    superblock_ = *sb;
    return true;
}

bool StorageManager::saveSuperblock() {
    if (!framAvailable_) return false;
    superblock_.crc16 = calculateCrc16(reinterpret_cast<uint8_t*>(&superblock_), sizeof(FramSuperblock) - sizeof(uint16_t));
    return fram_.write(ADDR_SUPERBLOCK, reinterpret_cast<uint8_t*>(&superblock_), sizeof(FramSuperblock));
}

uint16_t StorageManager::getPendingCount() const {
    if (superblock_.writeIndex >= superblock_.readIndex) {
        return superblock_.writeIndex - superblock_.readIndex;
    } else {
        return MAX_RECORDS - superblock_.readIndex + superblock_.writeIndex;
    }
}

bool StorageManager::appendRecord(const core::SensorSnapshot& snapshot, uint32_t uptimeMs) {
    if (!framAvailable_) return false;

    if (getPendingCount() >= MAX_RECORDS - 1) {
        // バッファフル。一番古いデータを上書きしてreadIndexを強制的に進める
        services::Logger::warn("StorageMgr", "FRAM Ring Buffer FULL. Overwriting oldest record.");
        superblock_.readIndex = (superblock_.readIndex + 1) % MAX_RECORDS;
    }

    PersistentRecordV4 rec;
    memset(&rec, 0, sizeof(PersistentRecordV4));
    
    rec.data.sequence = superblock_.nextSequence++;
    rec.data.uptimeMs = uptimeMs;
    
    rec.data.validFlags = 0;
    
    if (snapshot.environment.valid) {
        rec.data.temperatureC = snapshot.environment.temperatureC;
        rec.data.humidityRh = snapshot.environment.humidityRh;
        rec.data.validFlags |= VALID_TEMP | VALID_HUMIDITY;
        
        if (snapshot.environment.pressureValid) {
            rec.data.pressureHpa = snapshot.environment.pressureHpa;
            rec.data.validFlags |= VALID_PRESSURE;
        }
        
        if (snapshot.environment.altitudeValid) {
            rec.data.altitudeM = snapshot.environment.altitudeM;
            rec.data.validFlags |= VALID_ALTITUDE;
        }
        
        if (snapshot.environment.co2Ppm > 0) {
            rec.data.co2Ppm = snapshot.environment.co2Ppm;
            rec.data.validFlags |= VALID_CO2;
        }
        
        if (snapshot.environment.sgp41Valid) {
            rec.data.vocIndex = snapshot.environment.vocIndex;
            rec.data.noxIndex = snapshot.environment.noxIndex;
            rec.data.validFlags |= VALID_VOC | VALID_NOX;
        }
    }
    
    if (snapshot.ppg.state == core::PpgState::Measuring && snapshot.ppg.calculatedValid) {
        rec.data.heartRateBpm = snapshot.ppg.heartRateBpm;
        rec.data.spo2Percent = snapshot.ppg.spo2Percent;
        rec.data.validFlags |= VALID_HR | VALID_SPO2;
    }
    
    // GNSS Fields
    rec.data.sampleMonotonicUs = snapshot.gnss.sampleMonotonicUs;
    
    if (snapshot.gnss.timeValid) {
        rec.data.utcEpochMs = snapshot.gnss.utcEpochMs;
    } else {
        rec.data.utcEpochMs = 0;
    }

    if (snapshot.gnss.fixValid) {
        rec.data.gnssLatitudeE7 = static_cast<int32_t>(snapshot.gnss.latitudeDeg * 1e7);
        rec.data.gnssLongitudeE7 = static_cast<int32_t>(snapshot.gnss.longitudeDeg * 1e7);
        rec.data.gnssValidFlags |= GNSS_VALID_FIX;
    }
    
    if (snapshot.gnss.altitudeValid) {
        rec.data.gnssAltitudeMslM = snapshot.gnss.altitudeMslM;
        rec.data.gnssValidFlags |= GNSS_VALID_ALTITUDE;
    }
    
    if (snapshot.gnss.speedValid) {
        rec.data.gnssSpeedMps = snapshot.gnss.speedMps;
        rec.data.gnssValidFlags |= GNSS_VALID_SPEED;
    }
    
    if (snapshot.gnss.courseValid) {
        rec.data.gnssCourseDeg = snapshot.gnss.courseDeg;
        rec.data.gnssValidFlags |= GNSS_VALID_COURSE;
    }
    
    if (snapshot.gnss.hdopValid) {
        rec.data.gnssHdop = snapshot.gnss.hdop;
        rec.data.gnssValidFlags |= GNSS_VALID_HDOP;
    }
    
    if (snapshot.gnss.timeValid) {
        rec.data.gnssValidFlags |= GNSS_VALID_UTC;
    }

    rec.data.gnssSatellites = snapshot.gnss.satellites;
    rec.data.gnssAgeMs = snapshot.gnss.ageMs;

    // TimeSource status
    // Time source information needs to be retrieved, but since we don't have direct access
    // we use hal::Clock::source() directly here
    core::TimeSource ts = hal::Clock::source();
    rec.data.timeSource = static_cast<uint8_t>(ts);

    // Get time status from hal::Clock
    if (hal::Clock::isDisciplined()) {
        rec.data.gnssValidFlags |= GNSS_TIME_DISCIPLINED;
    }
    
    rec.header.sequence = rec.data.sequence;
    rec.header.length = sizeof(SensorRecordV4);
    rec.header.committed = 0; // まだコミットしない
    rec.header.crc = calculateCrc16(reinterpret_cast<uint8_t*>(&rec.data), sizeof(SensorRecordV4));

    uint16_t addr = ADDR_RING_BUFFER + (superblock_.writeIndex * RECORD_SLOT_SIZE);
    
    // 1. データを書き込む (committed = 0)
    if (!fram_.write(addr, reinterpret_cast<uint8_t*>(&rec), sizeof(PersistentRecordV4))) {
        services::Logger::error("StorageMgr", "Failed to write record to FRAM");
        return false;
    }
    
    // 2. コミットマーカーを書く (電源断対策)
    rec.header.committed = 1;
    if (!fram_.writeByte(addr + offsetof(PersistentRecordV4, header.committed), 1)) {
        services::Logger::error("StorageMgr", "Failed to commit record in FRAM");
        return false;
    }
    
    // 3. SuperblockのwriteIndexを進める
    superblock_.writeIndex = (superblock_.writeIndex + 1) % MAX_RECORDS;
    saveSuperblock();
    
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
    if (hal::Clock::isTimeSet()) {
        currentDateString_ = (targetDate.length() > 0) ? targetDate : hal::Clock::getFormattedDate();
        currentFilename_ = "/log_" + currentDateString_ + ".csv";
    } else {
        currentDateString_ = "";
        for (int i = 0; i < 1000; i++) {
            char filename[32];
            snprintf(filename, sizeof(filename), "/log_boot_%03d.csv", i);
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
        writeCsvHeader(file);
    }
    file.close();
    services::Logger::info("StorageMgr", "Target SD log file: %s", currentFilename_.c_str());
    return true;
}

void StorageManager::writeCsvHeader(File& file) {
    file.println("Sequence,UptimeMs,SampleMonotonicUs,TimestampUtc,TimeSource,CO2_ppm,Temp_C,RH_pct,Pressure_hPa,VOC_Index,NOx_Index,HR_bpm,SpO2_pct,BMP_Altitude_m,GNSS_Lat_deg,GNSS_Lon_deg,GNSS_AltMSL_m,GNSS_Speed_mps,GNSS_Course_deg,GNSS_Satellites,GNSS_HDOP,GNSS_FixValid,GNSS_TimeValid,GNSS_AgeMs,PPS_AgeMs,GNSS_TimeDisciplined,ValidFlags");
}

void StorageManager::formatCsvLine(char* buffer, size_t size, const SensorRecordV4& rec) {
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
    snprintf(buffer, size, "%lu,%lu,%lld,%s,%s,%.1f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.7f,%.7f,%.1f,%.1f,%.1f,%u,%.1f,%d,%d,%lu,%lu,%d,0x%02X",
             rec.sequence, rec.uptimeMs, rec.sampleMonotonicUs, timeStr, tsStr, 
             co2, temp, rh, press, voc, nox, hr, spo2, alt, 
             lat, lon, gnssAlt, speed, course, rec.gnssSatellites, hdop, 
             (rec.gnssValidFlags & GNSS_VALID_FIX) ? 1 : 0, 
             (rec.gnssValidFlags & GNSS_VALID_UTC) ? 1 : 0, 
             rec.gnssAgeMs, rec.ppsAgeMs, 
             (rec.gnssValidFlags & GNSS_TIME_DISCIPLINED) ? 1 : 0, 
             rec.validFlags);
}

void StorageManager::flushPendingToSd() {
    if (!framAvailable_) return;
    
    // Wi-Fiモード中はSDへの書き出しを停止し、FRAMにためておく
    // これによりWebサーバーからのファイルダウンロード時のSPIバス競合(WDTクラッシュなど)を完全に防ぐ
    if (wifiActive_) return;
    
    uint16_t pendingCount = getPendingCount();
    if (pendingCount == 0) return;

    lock(); // 排他制御開始

    // ローテーションチェック
    if (hal::Clock::isTimeSet()) {
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
            String tomorrowFilename = "/log_" + tomorrow + ".csv";
            
            if (!SD.exists(tomorrowFilename)) {
                services::Logger::info("StorageMgr", "Pre-creating tomorrow's file: %s", tomorrowFilename.c_str());
                File file = SD.open(tomorrowFilename.c_str(), FILE_WRITE);
                if (file) {
                    writeCsvHeader(file);
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

    File file = SD.open(currentFilename_.c_str(), FILE_APPEND);
    if (!file) {
        services::Logger::error("StorageMgr", "SD Write failed. Retrying later.");
        sdAvailable_ = false; // SD障害発生
        unlock();
        return;
    }

    uint16_t flushedCount = 0;
    uint16_t currentIndex = superblock_.readIndex;

    while (currentIndex != superblock_.writeIndex) {
        PersistentRecordV4 rec;
        uint16_t addr = ADDR_RING_BUFFER + (currentIndex * RECORD_SLOT_SIZE);
        
        if (fram_.read(addr, reinterpret_cast<uint8_t*>(&rec), sizeof(PersistentRecordV4))) {
            // Check CRC and committed
            if (rec.header.committed == 1) {
                uint16_t crc = calculateCrc16(reinterpret_cast<uint8_t*>(&rec.data), sizeof(SensorRecordV4));
                if (crc == rec.header.crc) {
                    char line[256];
                    formatCsvLine(line, sizeof(line), rec.data);
                    file.println(line);
                    flushedCount++;
                } else {
                    services::Logger::warn("StorageMgr", "CRC mismatch at slot %u (seq %lu)", currentIndex, rec.header.sequence);
                }
            } else {
                services::Logger::warn("StorageMgr", "Uncommitted record at slot %u", currentIndex);
            }
        }
        
        currentIndex = (currentIndex + 1) % MAX_RECORDS;
    }

    file.close();
    
    if (flushedCount > 0) {
        services::Logger::info("StorageMgr", "Flushed %u records to SD. Updating readIndex.", flushedCount);
        superblock_.readIndex = currentIndex;
        saveSuperblock();
    }
    
    unlock(); // 排他制御終了
}

} // namespace storage
