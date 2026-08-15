#pragma once

#include "storage/fram_storage.h"
#include "storage/storage_records.h"
#include "core/sensor_snapshot.h"
#include <SD.h>
#include <SPI.h>
#include <WString.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace storage {

class StorageManager {
public:
    StorageManager();

    bool begin();

    // センサ更新の記録 (FRAMへの追加)
    bool appendRecord(const core::SensorSnapshot& snapshot, uint32_t uptimeMs);

    // 未flushレコードのSDへの書き出し (バッチ処理)
    void flushPendingToSd();

    bool isSdAvailable() const { return sdAvailable_; }
    bool isFramAvailable() const { return framAvailable_; }
    uint16_t getPendingCount() const;
    uint16_t getMaxRecords() const { return MAX_RECORDS; }

    // Wi-Fiモード中はSDへの書き出しを一時停止するためのフラグ
    void setWifiActive(bool active) { wifiActive_ = active; }
    
    // 手動でFRAMの内容をSDに強制フラッシュする（Wi-Fi稼働中の最新データ取得用）
    void forceFlush();
    
    // 現在書き込み中のファイル名を取得 (削除保護などに使用)
    String getCurrentFilename() const { return currentFilename_; }
    
    // SGP41ベースライン保存・復元
    bool getSgp41States(float& voc0, float& voc1) const;
    void setSgp41States(float voc0, float voc1);

    // SCD41 calibration tracking
    uint32_t getScd41LastCalibrationEpoch() const;
    void setScd41LastCalibrationEpoch(uint32_t epoch);

    // BMP581 calibration tracking
    bool getBmp581Calibration(float& offsetHpa, uint32_t& epoch, float& tempC, float& slpHpa) const;
    void setBmp581Calibration(float offsetHpa, uint32_t epoch, float tempC, float slpHpa);

    // スレッドセーフなアクセスを提供するため
    void lock();
    void unlock();

private:
    FramStorage fram_;
    FramSuperblock superblock_;
    bool sdAvailable_;
    bool framAvailable_;
    bool wifiActive_ = false;
    String currentFilename_;
    String currentDateString_;
    uint32_t lastSdInitAttempt_;
    SemaphoreHandle_t mutex_;
    
    uint16_t calculateCrc16(const uint8_t* data, size_t length);
    void initSuperblock();
    bool loadSuperblock();
    bool saveSuperblock();
    
    bool initSdCard();
    bool createNewSdFile();
    void writeCsvHeader(File& file);
    void formatCsvLine(char* buffer, size_t size, const EnvironmentalRecord& rec);
};

} // namespace storage
