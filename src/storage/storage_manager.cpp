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
    if (sb->magic != FRAM_MAGIC || sb->formatVersion != FRAM_FORMAT_VERSION) {
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

    PersistentRecord rec;
    memset(&rec, 0, sizeof(PersistentRecord));
    
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
    
    rec.header.sequence = rec.data.sequence;
    rec.header.length = sizeof(EnvironmentalRecord);
    rec.header.committed = 0; // まだコミットしない
    rec.header.crc = calculateCrc16(reinterpret_cast<uint8_t*>(&rec.data), sizeof(EnvironmentalRecord));

    uint16_t addr = ADDR_RING_BUFFER + (superblock_.writeIndex * RECORD_SLOT_SIZE);
    
    // 1. データを書き込む (committed = 0)
    if (!fram_.write(addr, reinterpret_cast<uint8_t*>(&rec), sizeof(PersistentRecord))) {
        services::Logger::error("StorageMgr", "Failed to write record to FRAM");
        return false;
    }
    
    // 2. コミットマーカーを書く (電源断対策)
    rec.header.committed = 1;
    if (!fram_.writeByte(addr + offsetof(PersistentRecord, header.committed), 1)) {
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

    SPI.begin(SCK, MISO, MOSI, hal::pins::SD_CS);
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

bool StorageManager::createNewSdFile() {
    if (hal::Clock::isTimeSet()) {
        currentDateString_ = hal::Clock::getFormattedDate();
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
    file.println("Sequence,UptimeMs,Timestamp,CO2_ppm,Temp_C,RH_pct,Pressure_hPa,VOC_Index,NOx_Index,HR_bpm,SpO2_pct,Altitude_m,ValidFlags");
}

void StorageManager::formatCsvLine(char* buffer, size_t size, const EnvironmentalRecord& rec) {
    float temp = (rec.validFlags & VALID_TEMP) ? rec.temperatureC : NAN;
    float rh = (rec.validFlags & VALID_HUMIDITY) ? rec.humidityRh : NAN;
    float press = (rec.validFlags & VALID_PRESSURE) ? rec.pressureHpa : NAN;
    float co2 = (rec.validFlags & VALID_CO2) ? (float)rec.co2Ppm : NAN;
    float voc = (rec.validFlags & VALID_VOC) ? rec.vocIndex : NAN;
    float nox = (rec.validFlags & VALID_NOX) ? rec.noxIndex : NAN;
    float hr = (rec.validFlags & VALID_HR) ? rec.heartRateBpm : NAN;
    float spo2 = (rec.validFlags & VALID_SPO2) ? rec.spo2Percent : NAN;
    float alt = (rec.validFlags & VALID_ALTITUDE) ? rec.altitudeM : NAN;

    char timeStr[24] = "";
    if (hal::Clock::isTimeSet()) {
        uint32_t nowMs = millis();
        int32_t diffSec = (nowMs > rec.uptimeMs) ? ((nowMs - rec.uptimeMs) / 1000) : 0;
        
        // JST (+9時間) に補正
        time_t recordEpoch = hal::Clock::getEpoch() - diffSec + (9 * 3600);
        
        struct tm timeinfo;
        gmtime_r(&recordEpoch, &timeinfo);
        
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    snprintf(buffer, size, "%lu,%lu,%s,%.1f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,0x%02X",
             rec.sequence, rec.uptimeMs, timeStr, co2, temp, rh, press, voc, nox, hr, spo2, alt, rec.validFlags);
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
        String today = hal::Clock::getFormattedDate();
        if (today != currentDateString_) {
            services::Logger::info("StorageMgr", "Log rotation triggered: %s -> %s", 
                                   currentDateString_.c_str(), today.c_str());
            createNewSdFile();
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
        PersistentRecord rec;
        uint16_t addr = ADDR_RING_BUFFER + (currentIndex * RECORD_SLOT_SIZE);
        
        if (fram_.read(addr, reinterpret_cast<uint8_t*>(&rec), sizeof(PersistentRecord))) {
            // Check CRC and committed
            if (rec.header.committed == 1) {
                uint16_t crc = calculateCrc16(reinterpret_cast<uint8_t*>(&rec.data), sizeof(EnvironmentalRecord));
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
