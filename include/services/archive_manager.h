#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <WString.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
    ArchiveState state {ArchiveState::Idle};
    String currentFile;
    int processedFiles {0};
    int totalFiles {0};
    size_t processedBytes {0};
    size_t totalBytes {0};
    int progressPercent {0};
    String message;
    String outputFile;
    bool verified {false};
    int retainedOriginals {0};
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
    bool isFileBusy(const String& path) const;

    // 定期チェック (メインループ用、自動アーカイブのスケジューリング)
    void update(uint32_t nowMs);

    // Taskから呼ばれるループ実装
    void processArchiveTask();

private:
    storage::StorageManager& storageManager_;
    TaskHandle_t taskHandle_ = nullptr;
    
    // ステータスと制御
    std::atomic<ArchiveState> state_ {ArchiveState::Idle};
    std::atomic<bool> cancelRequested_ {false};
    mutable SemaphoreHandle_t statusMutex_ = nullptr;
    ArchiveStatus status_;

    // 対象ファイル情報
    std::vector<String> targetFiles_;
    String outputZipName_;
    bool isWeekly_ = false;
    
    // 自動アーカイブの管理
    uint32_t lastScheduleCheckMs_ = 0;
    uint32_t terminalStateSinceMs_ = 0;
    
    // ユーティリティ
    void setStatus(ArchiveState state, const String& message);
    void resetProgress();
    void updateProgress(const String& currentFile, int processedFiles,
                        size_t processedBytes);
    void checkWeeklySchedule(uint32_t nowMs);
    bool executeArchive();
    bool streamDeflateFile(const String& filename, void* pZip); // pZip is mz_zip_archive*
    bool validateArchive(const String& filename,
                         const std::vector<String>& expectedFiles);
    bool inspectTargets(size_t& totalBytes);
    int deleteVerifiedWeeklySources();
    void cleanupTmpZip();
    bool getFreeSpace(uint64_t& freeBytes);
    static bool isSafeArchivePath(const String& path);
};

} // namespace services
