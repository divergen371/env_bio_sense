#pragma once

#include "storage/fram_storage.h"
#include "storage/storage_records.h"
#include "core/sensor_snapshot.h"
#include <SD.h>
#include <SPI.h>
#include <WString.h>

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

private:
    FramStorage fram_;
    FramSuperblock superblock_;
    bool sdAvailable_;
    bool framAvailable_;
    String currentFilename_;
    uint32_t lastSdInitAttempt_;
    
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
