#include "services/archive_manager.h"
#include "services/logger.h"
#include "hal/clock.h"
#include "miniz.h"

namespace services {

constexpr size_t ARCHIVE_IO_BUFFER_SIZE = 16 * 1024;
constexpr uint32_t MINIMUM_FREE_SPACE_BYTES = 2 * 1024 * 1024; // 2MB min required

static size_t mz_write_callback(void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n) {
    File* pFile = static_cast<File*>(pOpaque);
    if (!pFile) return 0;
    
    // miniz may seek back to update headers
    if (pFile->position() != file_ofs) {
        if (!pFile->seek(file_ofs)) {
            return 0; // Seek failed
        }
    }
    
    // Write in chunks
    size_t written = 0;
    const uint8_t* pData = static_cast<const uint8_t*>(pBuf);
    while (written < n) {
        size_t toWrite = std::min(n - written, (size_t)4096);
        size_t bytes = pFile->write(pData + written, toWrite);
        if (bytes == 0) break;
        written += bytes;
    }
    return written;
}

struct ReadCallbackState {
    File* inFile;
    storage::StorageManager* storageMgr;
    ArchiveManager* archiveMgr;
};

static size_t mz_read_callback(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n) {
    ReadCallbackState* state = static_cast<ReadCallbackState*>(pOpaque);
    if (!state || !state->inFile) return 0;
    
    size_t totalRead = 0;
    uint8_t* pDest = static_cast<uint8_t*>(pBuf);
    
    // 16KiB単位で小ブロックreadし、こまめにMutexを解放してyieldする
    while (totalRead < n) {
        size_t toRead = std::min(n - totalRead, ARCHIVE_IO_BUFFER_SIZE);
        
        state->storageMgr->lock();
        if (state->inFile->position() != file_ofs + totalRead) {
            state->inFile->seek(file_ofs + totalRead);
        }
        size_t bytesRead = state->inFile->read(pDest + totalRead, toRead);
        state->storageMgr->unlock();
        
        if (bytesRead == 0) break;
        totalRead += bytesRead;
        
        // 進捗を更新してロギング側へ実行機会を譲る
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return totalRead;
}

ArchiveManager::ArchiveManager(storage::StorageManager& storageManager)
    : storageManager_(storageManager) {
}

void ArchiveManager::begin() {
    Logger::info("ArchiveMgr", "Initializing Archive Manager...");
    
    xTaskCreatePinnedToCore(
        [](void* arg) {
            ArchiveManager* mgr = static_cast<ArchiveManager*>(arg);
            mgr->processArchiveTask();
        },
        "ArchiveTask",
        8192,
        this,
        tskIDLE_PRIORITY + 1,
        &taskHandle_,
        0 // Core 0 (where WebServer runs)
    );
}

void ArchiveManager::setStatus(ArchiveState state, const String& message) {
    status_.state = state;
    status_.message = message;
    state_ = state;
    Logger::info("ArchiveMgr", "Status changed to %d: %s", (int)state, message.c_str());
}

ArchiveStatus ArchiveManager::getStatus() const {
    ArchiveStatus s = status_;
    return s;
}

bool ArchiveManager::getFreeSpace(size_t& freeBytes) {
    storageManager_.lock();
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    storageManager_.unlock();
    
    if (total == 0) return false;
    freeBytes = (size_t)(total - used);
    return true;
}

bool ArchiveManager::startManualArchive(const std::vector<String>& files) {
    if (state_ != ArchiveState::Idle) {
        return false;
    }
    
    if (files.empty()) return false;
    
    targetFiles_ = files;
    isWeekly_ = false;
    
    char buf[32];
    time_t nowEpoch = hal::Clock::getEpoch() + (9 * 3600); // JST
    struct tm timeinfo;
    gmtime_r(&nowEpoch, &timeinfo);
    snprintf(buf, sizeof(buf), "manual_%04d%02d%02d_%02d%02d%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
             
    outputZipName_ = "/" + String(buf) + ".zip";
    cancelRequested_ = false;
    
    setStatus(ArchiveState::Preparing, "Manual archive scheduled");
    return true;
}

void ArchiveManager::cancelArchive() {
    if (state_ != ArchiveState::Idle && state_ != ArchiveState::Completed && state_ != ArchiveState::Failed) {
        cancelRequested_ = true;
    }
}

void ArchiveManager::update(uint32_t nowMs) {
    // スケジューリング処理
    if (nowMs - lastScheduleCheckMs_ > 60000) { // 1分ごとにチェック
        lastScheduleCheckMs_ = nowMs;
        checkWeeklySchedule(nowMs);
    }
}

void ArchiveManager::checkWeeklySchedule(uint32_t nowMs) {
    if (state_ != ArchiveState::Idle) return;
    if (!hal::Clock::isTimeSet()) return;
    
    time_t nowEpoch = hal::Clock::getEpoch() + (9 * 3600); // JST
    struct tm timeinfo;
    gmtime_r(&nowEpoch, &timeinfo);
    
    // 月曜日の 03:00 〜 06:00
    if (timeinfo.tm_wday == 1 && timeinfo.tm_hour >= 3 && timeinfo.tm_hour < 6) {
        // 先週月曜日〜日曜日の日付を算出
        time_t lastMonEpoch = nowEpoch - (7 * 86400) - (timeinfo.tm_hour * 3600) - (timeinfo.tm_min * 60) - timeinfo.tm_sec;
        time_t lastSunEpoch = lastMonEpoch + (6 * 86400);
        
        struct tm tmStart, tmEnd;
        gmtime_r(&lastMonEpoch, &tmStart);
        gmtime_r(&lastSunEpoch, &tmEnd);
        
        char startStr[16], endStr[16];
        snprintf(startStr, sizeof(startStr), "%04d%02d%02d", tmStart.tm_year + 1900, tmStart.tm_mon + 1, tmStart.tm_mday);
        snprintf(endStr, sizeof(endStr), "%04d%02d%02d", tmEnd.tm_year + 1900, tmEnd.tm_mon + 1, tmEnd.tm_mday);
        
        String zipName = "/weekly_" + String(startStr) + "_" + String(endStr) + ".zip";
        
        storageManager_.lock();
        bool exists = SD.exists(zipName.c_str());
        storageManager_.unlock();
        
        if (!exists) {
            // 対象ファイルを列挙
            std::vector<String> candidates;
            storageManager_.lock();
            File root = SD.open("/");
            if (root) {
                File file = root.openNextFile();
                while (file) {
                    String name = file.name();
                    if (!file.isDirectory() && name.startsWith("log_") && name.endsWith(".csv")) {
                        // "log_YYYYMMDD.csv" -> YYYYMMDD
                        String datePart = name.substring(4, 12);
                        if (datePart >= startStr && datePart <= endStr) {
                            String fullPath = "/" + name;
                            if (fullPath != storageManager_.getCurrentFilename()) {
                                candidates.push_back(fullPath);
                            }
                        }
                    }
                    file = root.openNextFile();
                }
                root.close();
            }
            storageManager_.unlock();
            
            if (!candidates.empty()) {
                targetFiles_ = candidates;
                isWeekly_ = true;
                outputZipName_ = zipName;
                cancelRequested_ = false;
                setStatus(ArchiveState::Preparing, "Weekly archive scheduled");
            }
        }
    }
}

void ArchiveManager::processArchiveTask() {
    while (true) {
        if (state_ == ArchiveState::Preparing) {
            bool success = executeArchive();
            if (success) {
                setStatus(ArchiveState::Completed, "Archive completed successfully");
                if (isWeekly_) {
                    // 自動圧縮完了後、元CSVの削除
                    storageManager_.lock();
                    for (const auto& file : targetFiles_) {
                        SD.remove(file.c_str());
                    }
                    storageManager_.unlock();
                    Logger::info("ArchiveMgr", "Deleted original CSVs after weekly archive");
                }
            } else if (cancelRequested_) {
                setStatus(ArchiveState::Failed, "Archive cancelled by user");
            } else {
                setStatus(ArchiveState::Failed, "Archive failed");
            }
            
            targetFiles_.clear();
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool ArchiveManager::executeArchive() {
    size_t freeBytes = 0;
    if (!getFreeSpace(freeBytes) || freeBytes < MINIMUM_FREE_SPACE_BYTES) {
        setStatus(ArchiveState::Failed, "Insufficient SD free space");
        return false;
    }
    
    // .tmp.zip をクリーンアップ
    cleanupTmpZip();
    
    String tmpZipName = outputZipName_;
    tmpZipName.replace(".zip", ".tmp.zip");
    
    storageManager_.lock();
    File zipFile = SD.open(tmpZipName.c_str(), FILE_WRITE);
    storageManager_.unlock();
    
    if (!zipFile) {
        Logger::error("ArchiveMgr", "Failed to create %s", tmpZipName.c_str());
        return false;
    }
    
    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));
    
    // miniz の write コールバックを自作に設定
    zip_archive.m_pWrite = mz_write_callback;
    zip_archive.m_pIO_opaque = &zipFile;

    if (!mz_zip_writer_init_v2(&zip_archive, 0, 0)) {
        zipFile.close();
        return false;
    }
    
    status_.totalFiles = targetFiles_.size();
    status_.processedFiles = 0;
    
    setStatus(ArchiveState::Compressing, "Compressing files...");
    
    for (const String& file : targetFiles_) {
        if (cancelRequested_) break;
        
        status_.currentFile = file;
        
        if (!streamDeflateFile(file, &zip_archive)) {
            Logger::error("ArchiveMgr", "Failed to deflate %s", file.c_str());
            mz_zip_writer_end(&zip_archive);
            zipFile.close();
            return false;
        }
        
        status_.processedFiles++;
    }
    
    setStatus(ArchiveState::Verifying, "Finalizing ZIP...");
    
    if (!mz_zip_writer_finalize_archive(&zip_archive)) {
        mz_zip_writer_end(&zip_archive);
        zipFile.close();
        return false;
    }
    
    mz_zip_writer_end(&zip_archive);
    
    storageManager_.lock();
    zipFile.close();
    storageManager_.unlock();
    
    if (cancelRequested_) {
        cleanupTmpZip();
        return false;
    }
    
    // rename
    storageManager_.lock();
    if (SD.exists(outputZipName_.c_str())) {
        SD.remove(outputZipName_.c_str());
    }
    bool renameOk = SD.rename(tmpZipName.c_str(), outputZipName_.c_str());
    storageManager_.unlock();
    
    if (!renameOk) {
        Logger::error("ArchiveMgr", "Failed to rename tmp zip");
        return false;
    }
    
    Logger::info("ArchiveMgr", "Successfully created archive: %s", outputZipName_.c_str());
    return true;
}

bool ArchiveManager::streamDeflateFile(const String& filename, void* pZip) {
    mz_zip_archive* zip = static_cast<mz_zip_archive*>(pZip);
    
    storageManager_.lock();
    File inFile = SD.open(filename.c_str(), FILE_READ);
    storageManager_.unlock();
    
    if (!inFile) return false;
    
    size_t fileSize = inFile.size();
    status_.totalBytes = fileSize;
    status_.processedBytes = 0;
    status_.progressPercent = 0;
    
    String entryName = filename;
    if (entryName.startsWith("/")) {
        entryName = entryName.substring(1); // 先頭の '/' を除去
    }
    
    // miniz のストリーミングAPIを使用し、自作のコールバックを通じて読み込む
    ReadCallbackState readState = { &inFile, &storageManager_, this };
    
    mz_uint level_and_flags = MZ_DEFAULT_LEVEL; // level 6
    
    bool ok = mz_zip_writer_add_read_buf_callback(
        zip, 
        entryName.c_str(), 
        mz_read_callback, 
        &readState, 
        fileSize, 
        nullptr, 
        nullptr, 
        0, 
        level_and_flags,
        nullptr,
        0,
        nullptr,
        0
    );
    
    storageManager_.lock();
    inFile.close();
    storageManager_.unlock();
    
    return ok;
}

void ArchiveManager::cleanupTmpZip() {
    String tmpZipName = outputZipName_;
    tmpZipName.replace(".zip", ".tmp.zip");
    storageManager_.lock();
    if (SD.exists(tmpZipName.c_str())) {
        SD.remove(tmpZipName.c_str());
    }
    storageManager_.unlock();
}

} // namespace services
