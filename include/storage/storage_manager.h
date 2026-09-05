#pragma once

#include "storage/fram_storage.h"
#include "storage/fram_wal.h"
#include "storage/sd_transaction.h"
#include "storage/storage_records.h"
#include "core/sensor_snapshot.h"
#include <SD.h>
#include <SPI.h>
#include <WString.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>

namespace storage {

class StorageManager {
public:
    StorageManager();

    bool begin();

    // センサ更新の記録 (FRAMへの追加)
    bool appendRecord(const core::SensorSnapshot& snapshot, uint32_t uptimeMs);

    // 未flushレコードのSDへの書き出し (バッチ処理)
    void flushPendingToSd();

    bool isSdAvailable() const { return sdAvailable_.load(); }
    bool isFramAvailable() const { return framAvailable_.load(); }
    bool isFramReadOnly() const { return framReadOnly_.load(); }
    uint16_t getPendingCount() const;
    uint16_t getMaxRecords() const { return MAX_RECORDS; }
    FramWalStats getWalStats() const;

    // Compact abnormal-state journal. Records survive reboot and are kept in a
    // separate circular area so routine CSV samples do not crowd them out.
    bool appendEvent(EventCode code, int32_t detail, uint32_t uptimeMs);
    size_t readRecentEvents(EventRecord* out, size_t maxCount);
    uint16_t getEventCount() const;
    uint16_t getMaxEventRecords() const { return MAX_EVENT_RECORDS; }

    // Wi-Fiモード中はSDへの書き出しを一時停止するためのフラグ
    void setWifiActive(bool active) { wifiActive_.store(active); }
    
    // 手動でFRAMの内容をSDに強制フラッシュする（Wi-Fi稼働中の最新データ取得用）
    void forceFlush();
    
    // 現在書き込み中のファイル名を取得 (削除保護などに使用)
    String getCurrentFilename() const;
    
    // SGP41ベースライン保存・復元
    bool getSgp41States(float& voc0, float& voc1) const;
    void setSgp41States(float voc0, float voc1);

    // SCD41 calibration tracking
    uint32_t getScd41LastCalibrationEpoch() const;
    void setScd41LastCalibrationEpoch(uint32_t epoch);

    // AMeDAS field provenance log. The caller supplies one RFC4180-safe row;
    // the fixed header/path and write verification stay inside storage.
    bool appendAmedasLogLine(const char* line);
    bool appendBmp581CalibrationLogLine(const char* line);

    // BMP581 calibration tracking
    bool getBmp581Calibration(float& offsetHpa, uint32_t& epoch, float& tempC, float& slpHpa) const;
    bool setBmp581Calibration(float offsetHpa, uint32_t epoch, float tempC, float slpHpa);

    // スレッドセーフなアクセスを提供するため
    void lock() const;
    void unlock() const;

private:
    FramStorage fram_;
    FramWal<FramStorage> wal_;
    SdTransactionJournal<FramStorage> sdTransaction_;
    FramSuperblock superblock_;
    FramWalStats walStats_ {};
    std::atomic<bool> sdAvailable_;
    std::atomic<bool> framAvailable_;
    std::atomic<bool> framReadOnly_ {false};
    std::atomic<bool> wifiActive_ {false};
    String currentFilename_;
    String currentDateString_;
    uint32_t lastSdInitAttempt_;
    mutable SemaphoreHandle_t mutex_;
    uint32_t lastRingFullEventMs_ {0};

    uint16_t eventWriteIndex_ {0};
    uint16_t eventCount_ {0};
    uint32_t nextEventSequence_ {1};
    
    uint16_t calculateCrc16(const uint8_t* data, size_t length);
    void initSuperblock();
    bool loadSuperblock();
    bool saveSuperblock();
    void loadEventJournal();
    bool readEventSlot(uint16_t index, EventRecord& record);
    
    bool initSdCard();
    bool createNewSdFile(const String& targetDate = "");
    bool appendAndVerifyCsvLine(const char* line, size_t lineLength);
    bool appendVerifiedDiagnosticCsv(const char* path, const char* header,
                                     const char* line);
    bool quarantineCorruptTail(uint16_t slotIndex, uint32_t uptimeMs);
    bool appendAndVerifyQuarantine(const FramQuarantineRecord& record);
    uint16_t activeCsvSchemaVersion() const;
    void writeCsvHeader(File& file, uint16_t schemaVersion);
    void formatCsvLineV5(char* buffer, size_t size, const SensorRecordV5& rec);
};

} // namespace storage
