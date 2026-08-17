#pragma once

#include <vector>
#include <WString.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "storage/storage_manager.h"
#include <SD.h>

namespace services {

enum class ArchiveState {
    Idle,
    Preparing,
    Compressing,
    Verifying,
    Completed,
    Failed
};

struct ArchiveStatus {
    ArchiveState state;
    String currentFile;
    int processedFiles;
    int totalFiles;
    size_t processedBytes;
    size_t totalBytes;
    int progressPercent;
    String message;
};

class ArchiveManager {
public:
    ArchiveManager(storage::StorageManager& storageManager);
    
    // 初期化とArchiveTaskの起動
    void begin();

    // 手動アーカイブの開始要求
    bool startManualArchive(const std::vector<String>& files);

    // アーカイブのキャンセル
    void cancelArchive();

    // 現在のステータス取得
    ArchiveStatus getStatus() const;

    // 定期チェック (メインループ用、自動アーカイブのスケジューリング)
    void update(uint32_t nowMs);

    // Taskから呼ばれるループ実装
    void processArchiveTask();

private:
    storage::StorageManager& storageManager_;
    TaskHandle_t taskHandle_ = nullptr;
    
    // ステータスと制御
    volatile ArchiveState state_ = ArchiveState::Idle;
    volatile bool cancelRequested_ = false;
    ArchiveStatus status_;

    // 対象ファイル情報
    std::vector<String> targetFiles_;
    String outputZipName_;
    bool isWeekly_ = false;
    
    // 自動アーカイブの管理
    uint32_t lastScheduleCheckMs_ = 0;
    
    // ユーティリティ
    void setStatus(ArchiveState state, const String& message);
    void checkWeeklySchedule(uint32_t nowMs);
    bool executeArchive();
    bool streamDeflateFile(const String& filename, void* pZip); // pZip is mz_zip_archive*
    void cleanupTmpZip();
    bool getFreeSpace(size_t& freeBytes);
};

} // namespace services
