#include "services/archive_manager.h"
#include "services/logger.h"
#include "hal/clock.h"
#include "miniz.h"
#include <algorithm>
#include <cstring>

namespace services {
namespace {

constexpr size_t ARCHIVE_IO_BUFFER_SIZE = 16 * 1024;
constexpr uint32_t MINIMUM_FREE_SPACE_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_MANUAL_FILES = 64;
constexpr uint32_t TERMINAL_STATUS_HOLD_MS = 5000;

struct ZipIoState {
    File* file;
    storage::StorageManager* storage;
};

size_t zipWriteCallback(void* opaque, mz_uint64 fileOffset,
                        const void* buffer, size_t length) {
    ZipIoState* state = static_cast<ZipIoState*>(opaque);
    if (state == nullptr || state->file == nullptr ||
        state->storage == nullptr || buffer == nullptr) {
        return 0;
    }
    size_t written = 0;
    const uint8_t* source = static_cast<const uint8_t*>(buffer);
    while (written < length) {
        const size_t chunk = std::min<size_t>(length - written, 4096);
        state->storage->lock();
        bool positioned = state->file->position() == fileOffset + written;
        if (!positioned) positioned = state->file->seek(fileOffset + written);
        const size_t actual = positioned
            ? state->file->write(source + written, chunk) : 0;
        state->storage->unlock();
        if (actual != chunk) break;
        written += actual;
        taskYIELD();
    }
    return written;
}

size_t zipReadCallback(void* opaque, mz_uint64 fileOffset,
                       void* buffer, size_t length) {
    ZipIoState* state = static_cast<ZipIoState*>(opaque);
    if (state == nullptr || state->file == nullptr ||
        state->storage == nullptr || buffer == nullptr) {
        return 0;
    }
    size_t totalRead = 0;
    uint8_t* destination = static_cast<uint8_t*>(buffer);
    while (totalRead < length) {
        const size_t chunk = std::min(
            length - totalRead, ARCHIVE_IO_BUFFER_SIZE);
        state->storage->lock();
        bool positioned =
            state->file->position() == fileOffset + totalRead;
        if (!positioned) {
            positioned = state->file->seek(fileOffset + totalRead);
        }
        const size_t actual = positioned
            ? state->file->read(destination + totalRead, chunk) : 0;
        state->storage->unlock();
        if (actual == 0) break;
        totalRead += actual;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return totalRead;
}

bool hasAllowedArchiveSuffix(const String& path) {
    return path.endsWith(".csv") || path.endsWith(".json") ||
           path.endsWith(".ppg") || path.endsWith(".zip") ||
           path.endsWith(".incomplete");
}

} // namespace

ArchiveManager::ArchiveManager(storage::StorageManager& storageManager)
    : storageManager_(storageManager) {
    statusMutex_ = xSemaphoreCreateMutex();
    status_.state = ArchiveState::Idle;
    status_.message = "Idle";
}

void ArchiveManager::begin() {
    Logger::info("ArchiveMgr", "Initializing Archive Manager...");
    xTaskCreatePinnedToCore(
        [](void* arg) {
            static_cast<ArchiveManager*>(arg)->processArchiveTask();
        },
        "ArchiveTask", 8192, this, tskIDLE_PRIORITY + 1,
        &taskHandle_, 0);
}

void ArchiveManager::setStatus(ArchiveState state,
                               const String& message) {
    if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
    status_.state = state;
    status_.message = message;
    if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
    state_.store(state);
    Logger::info("ArchiveMgr", "Status changed to %d: %s",
                 static_cast<int>(state), message.c_str());
}

void ArchiveManager::resetProgress() {
    if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
    status_.currentFile = "";
    status_.processedFiles = 0;
    status_.totalFiles = static_cast<int>(targetFiles_.size());
    status_.processedBytes = 0;
    status_.totalBytes = 0;
    status_.progressPercent = 0;
    status_.outputFile = outputZipName_;
    status_.verified = false;
    status_.retainedOriginals = 0;
    if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
}

void ArchiveManager::updateProgress(const String& currentFile,
                                    int processedFiles,
                                    size_t processedBytes) {
    if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
    status_.currentFile = currentFile;
    status_.processedFiles = processedFiles;
    status_.processedBytes = processedBytes;
    status_.progressPercent = status_.totalBytes == 0 ? 0
        : static_cast<int>(std::min<size_t>(100,
            static_cast<size_t>((static_cast<uint64_t>(processedBytes) * 100u) /
                                status_.totalBytes)));
    if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
}

ArchiveStatus ArchiveManager::getStatus() const {
    if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
    const ArchiveStatus copy = status_;
    if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
    return copy;
}

bool ArchiveManager::isFileBusy(const String& path) const {
    const ArchiveState current = state_.load();
    if (current != ArchiveState::Preparing &&
        current != ArchiveState::Compressing &&
        current != ArchiveState::Verifying) {
        return false;
    }
    bool busy = false;
    if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
    for (const String& target : targetFiles_) {
        if (target == path) {
            busy = true;
            break;
        }
    }
    if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
    return busy;
}

bool ArchiveManager::getFreeSpace(uint64_t& freeBytes) {
    storageManager_.lock();
    const uint64_t total = SD.totalBytes();
    const uint64_t used = SD.usedBytes();
    storageManager_.unlock();
    if (total == 0 || used > total) return false;
    freeBytes = total - used;
    return true;
}

bool ArchiveManager::isSafeArchivePath(const String& path) {
    if (path.length() < 2 || path.length() > 120 || path[0] != '/' ||
        path.indexOf("..") >= 0 || path.indexOf('\\') >= 0 ||
        path.indexOf("//") >= 0 || path.endsWith(".tmp") ||
        path.endsWith(".tmp.zip") || !hasAllowedArchiveSuffix(path)) {
        return false;
    }
    for (size_t i = 1; i < path.length(); ++i) {
        const char c = path[i];
        const bool allowed = (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '_' || c == '-' || c == '.';
        if (!allowed) return false;
    }
    const int secondSlash = path.indexOf('/', 1);
    return secondSlash < 0 || path.startsWith("/data/ppg/") ||
           path.startsWith("/data/system/");
}

bool ArchiveManager::startManualArchive(
        const std::vector<String>& files) {
    const ArchiveState current = state_.load();
    if (current != ArchiveState::Idle) return false;
    if (files.empty() || files.size() > MAX_MANUAL_FILES) return false;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!isSafeArchivePath(files[i])) return false;
        for (size_t j = 0; j < i; ++j) {
            if (files[i] == files[j]) return false;
        }
    }

    char name[48] {};
    if (hal::Clock::isTimeSet()) {
        const time_t nowEpoch = hal::Clock::getEpoch() + 9 * 3600;
        struct tm timeinfo {};
        gmtime_r(&nowEpoch, &timeinfo);
        std::snprintf(name, sizeof(name),
            "manual_%04d%02d%02d_%02d%02d%02d_%04lX.zip",
            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
            timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
            timeinfo.tm_sec, static_cast<unsigned long>(millis() & 0xFFFFu));
    } else {
        std::snprintf(name, sizeof(name), "manual_boot_%08lX.zip",
            static_cast<unsigned long>(millis()));
    }

    if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
    const ArchiveState lockedState = state_.load();
    if (lockedState != ArchiveState::Idle) {
        if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
        return false;
    }
    targetFiles_ = files;
    outputZipName_ = "/" + String(name);
    isWeekly_ = false;
    cancelRequested_.store(false);
    terminalStateSinceMs_ = 0;
    status_ = ArchiveStatus();
    status_.state = ArchiveState::Preparing;
    status_.message = "Manual archive scheduled";
    status_.totalFiles = static_cast<int>(files.size());
    status_.outputFile = outputZipName_;
    state_.store(ArchiveState::Preparing);
    if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
    return true;
}

void ArchiveManager::cancelArchive() {
    const ArchiveState current = state_.load();
    if (current == ArchiveState::Preparing ||
        current == ArchiveState::Compressing ||
        current == ArchiveState::Verifying) {
        cancelRequested_.store(true);
    }
}

void ArchiveManager::update(uint32_t nowMs) {
    if (nowMs - lastScheduleCheckMs_ > 60000u) {
        lastScheduleCheckMs_ = nowMs;
        checkWeeklySchedule(nowMs);
    }
}

void ArchiveManager::checkWeeklySchedule(uint32_t) {
    if (state_.load() != ArchiveState::Idle ||
        !hal::Clock::isTimeSet()) return;

    const time_t nowEpoch = hal::Clock::getEpoch() + 9 * 3600;
    struct tm now {};
    gmtime_r(&nowEpoch, &now);
    if (now.tm_wday != 1 || now.tm_hour < 3 || now.tm_hour >= 6) return;

    const time_t lastMonday = nowEpoch - 7 * 86400 -
        now.tm_hour * 3600 - now.tm_min * 60 - now.tm_sec;
    const time_t lastSunday = lastMonday + 6 * 86400;
    struct tm start {};
    struct tm end {};
    gmtime_r(&lastMonday, &start);
    gmtime_r(&lastSunday, &end);
    char startText[16] {};
    char endText[16] {};
    std::snprintf(startText, sizeof(startText), "%04d%02d%02d",
        start.tm_year + 1900, start.tm_mon + 1, start.tm_mday);
    std::snprintf(endText, sizeof(endText), "%04d%02d%02d",
        end.tm_year + 1900, end.tm_mon + 1, end.tm_mday);
    const String zipName = "/weekly_" + String(startText) + "_" +
        String(endText) + ".zip";

    std::vector<String> candidates;
    const String currentLog = storageManager_.getCurrentFilename();
    storageManager_.lock();
    const bool archiveExists = SD.exists(zipName.c_str());
    if (!archiveExists) {
        File root = SD.open("/");
        File file = root ? root.openNextFile() : File();
        while (file) {
            const String name = file.name();
            const int slash = name.lastIndexOf('/');
            const String basename = slash >= 0
                ? name.substring(slash + 1) : name;
            if (!file.isDirectory() && basename.startsWith("log_") &&
                basename.endsWith(".csv")) {
                const String datePart = basename.substring(4, 12);
                String fullPath = name.startsWith("/") ? name : "/" + name;
                if (datePart >= startText && datePart <= endText &&
                    fullPath != currentLog) {
                    candidates.push_back(fullPath);
                }
            }
            file.close();
            file = root.openNextFile();
        }
        if (root) root.close();
    }
    storageManager_.unlock();
    if (archiveExists || candidates.empty()) return;

    if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
    if (state_.load() != ArchiveState::Idle) {
        if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
        return;
    }
    targetFiles_ = candidates;
    outputZipName_ = zipName;
    isWeekly_ = true;
    cancelRequested_.store(false);
    terminalStateSinceMs_ = 0;
    status_ = ArchiveStatus();
    status_.state = ArchiveState::Preparing;
    status_.message = "Weekly archive scheduled";
    status_.totalFiles = static_cast<int>(candidates.size());
    status_.outputFile = outputZipName_;
    state_.store(ArchiveState::Preparing);
    if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
}

void ArchiveManager::processArchiveTask() {
    while (true) {
        if (state_.load() == ArchiveState::Preparing) {
            resetProgress();
            const bool success = executeArchive();
            if (success) {
                int retained = 0;
                if (isWeekly_) retained = deleteVerifiedWeeklySources();
                if (statusMutex_ != nullptr) {
                    xSemaphoreTake(statusMutex_, portMAX_DELAY);
                }
                status_.verified = true;
                status_.retainedOriginals = retained;
                if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
                setStatus(ArchiveState::Completed,
                    retained == 0
                        ? "Archive verified successfully"
                        : "Archive verified; some originals were retained");
            } else if (cancelRequested_.load()) {
                setStatus(ArchiveState::Failed, "Archive cancelled by user");
            } else {
                setStatus(ArchiveState::Failed, "Archive failed verification");
            }
            if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
            targetFiles_.clear();
            if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);
            terminalStateSinceMs_ = millis();
        } else {
            const ArchiveState current = state_.load();
            if ((current == ArchiveState::Completed ||
                 current == ArchiveState::Failed) &&
                terminalStateSinceMs_ != 0 &&
                millis() - terminalStateSinceMs_ >= TERMINAL_STATUS_HOLD_MS) {
                setStatus(ArchiveState::Idle, "Idle");
                terminalStateSinceMs_ = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

bool ArchiveManager::inspectTargets(size_t& totalBytes) {
    totalBytes = 0;
    const String currentLog = storageManager_.getCurrentFilename();
    storageManager_.lock();
    for (const String& path : targetFiles_) {
        if (!isSafeArchivePath(path) || path == currentLog ||
            !SD.exists(path.c_str())) {
            storageManager_.unlock();
            return false;
        }
        File file = SD.open(path.c_str(), FILE_READ);
        if (!file || file.isDirectory()) {
            if (file) file.close();
            storageManager_.unlock();
            return false;
        }
        const size_t fileSize = file.size();
        if (fileSize > SIZE_MAX - totalBytes) {
            file.close();
            storageManager_.unlock();
            return false;
        }
        totalBytes += fileSize;
        file.close();
    }
    storageManager_.unlock();
    return totalBytes > 0;
}

bool ArchiveManager::executeArchive() {
    size_t totalBytes = 0;
    uint64_t freeBytes = 0;
    if (!inspectTargets(totalBytes) ||
        totalBytes > SIZE_MAX - MINIMUM_FREE_SPACE_BYTES ||
        !getFreeSpace(freeBytes) ||
        freeBytes < static_cast<uint64_t>(totalBytes) +
            MINIMUM_FREE_SPACE_BYTES) {
        Logger::error("ArchiveMgr", "Targets invalid or SD free space insufficient");
        return false;
    }
    if (statusMutex_ != nullptr) xSemaphoreTake(statusMutex_, portMAX_DELAY);
    status_.totalBytes = totalBytes;
    if (statusMutex_ != nullptr) xSemaphoreGive(statusMutex_);

    cleanupTmpZip();
    String tmpZipName = outputZipName_;
    tmpZipName.replace(".zip", ".tmp.zip");
    storageManager_.lock();
    const bool finalCollision = SD.exists(outputZipName_.c_str());
    File zipFile = finalCollision
        ? File() : SD.open(tmpZipName.c_str(), FILE_WRITE);
    storageManager_.unlock();
    if (!zipFile || finalCollision) {
        Logger::error("ArchiveMgr", "Archive output path collision or create failure");
        return false;
    }

    ZipIoState writeState {&zipFile, &storageManager_};
    mz_zip_archive archive {};
    archive.m_pWrite = zipWriteCallback;
    archive.m_pIO_opaque = &writeState;
    if (!mz_zip_writer_init_v2(&archive, 0, 0)) {
        storageManager_.lock();
        zipFile.close();
        storageManager_.unlock();
        return false;
    }

    setStatus(ArchiveState::Compressing, "Compressing files");
    size_t processedBytes = 0;
    int processedFiles = 0;
    bool ok = true;
    for (const String& path : targetFiles_) {
        if (cancelRequested_.load()) {
            ok = false;
            break;
        }
        updateProgress(path, processedFiles, processedBytes);
        if (!streamDeflateFile(path, &archive)) {
            Logger::error("ArchiveMgr", "Failed to deflate %s", path.c_str());
            ok = false;
            break;
        }
        storageManager_.lock();
        File input = SD.open(path.c_str(), FILE_READ);
        const size_t size = input ? input.size() : 0;
        if (input) input.close();
        storageManager_.unlock();
        processedBytes += size;
        ++processedFiles;
        updateProgress(path, processedFiles, processedBytes);
    }

    if (ok) {
        setStatus(ArchiveState::Verifying, "Finalizing and verifying ZIP");
        ok = mz_zip_writer_finalize_archive(&archive);
    }
    mz_zip_writer_end(&archive);
    storageManager_.lock();
    zipFile.flush();
    zipFile.close();
    storageManager_.unlock();
    if (!ok || cancelRequested_.load()) {
        cleanupTmpZip();
        return false;
    }

    if (!validateArchive(tmpZipName, targetFiles_)) {
        Logger::error("ArchiveMgr", "Temporary ZIP failed full validation");
        return false;
    }
    storageManager_.lock();
    const bool renamed = !SD.exists(outputZipName_.c_str()) &&
        SD.rename(tmpZipName.c_str(), outputZipName_.c_str());
    storageManager_.unlock();
    if (!renamed) return false;

    if (!validateArchive(outputZipName_, targetFiles_)) {
        String invalidPath = outputZipName_ + ".invalid." + String(millis());
        storageManager_.lock();
        SD.rename(outputZipName_.c_str(), invalidPath.c_str());
        storageManager_.unlock();
        Logger::error("ArchiveMgr", "Final ZIP failed read-back validation");
        return false;
    }
    updateProgress("", processedFiles, processedBytes);
    Logger::info("ArchiveMgr", "Verified archive: %s",
                 outputZipName_.c_str());
    return true;
}

bool ArchiveManager::streamDeflateFile(const String& filename,
                                       void* zipPointer) {
    mz_zip_archive* archive = static_cast<mz_zip_archive*>(zipPointer);
    storageManager_.lock();
    File input = SD.open(filename.c_str(), FILE_READ);
    const size_t fileSize = input ? input.size() : 0;
    storageManager_.unlock();
    if (!input || fileSize == 0) {
        if (input) {
            storageManager_.lock();
            input.close();
            storageManager_.unlock();
        }
        return false;
    }

    String entryName = filename.startsWith("/")
        ? filename.substring(1) : filename;
    ZipIoState readState {&input, &storageManager_};
    const bool ok = mz_zip_writer_add_read_buf_callback(
        archive, entryName.c_str(), zipReadCallback, &readState,
        fileSize, nullptr, nullptr, 0, MZ_DEFAULT_LEVEL,
        nullptr, 0, nullptr, 0);
    storageManager_.lock();
    input.close();
    storageManager_.unlock();
    return ok;
}

bool ArchiveManager::validateArchive(
        const String& filename,
        const std::vector<String>& expectedFiles) {
    storageManager_.lock();
    File input = SD.open(filename.c_str(), FILE_READ);
    const size_t archiveSize = input ? input.size() : 0;
    storageManager_.unlock();
    if (!input || archiveSize == 0) {
        if (input) {
            storageManager_.lock();
            input.close();
            storageManager_.unlock();
        }
        return false;
    }

    ZipIoState readState {&input, &storageManager_};
    mz_zip_archive archive {};
    archive.m_pRead = zipReadCallback;
    archive.m_pIO_opaque = &readState;
    bool initialized = mz_zip_reader_init(&archive, archiveSize, 0);
    bool ok = initialized &&
        mz_zip_reader_get_num_files(&archive) == expectedFiles.size();
    for (const String& path : expectedFiles) {
        if (!ok) break;
        const String entryName = path.startsWith("/")
            ? path.substring(1) : path;
        const int index = mz_zip_reader_locate_file(
            &archive, entryName.c_str(), nullptr,
            MZ_ZIP_FLAG_CASE_SENSITIVE);
        mz_zip_archive_file_stat entry {};
        ok = index >= 0 &&
            mz_zip_reader_file_stat(&archive, index, &entry) &&
            !entry.m_is_directory;
        if (!ok) break;
        storageManager_.lock();
        File source = SD.open(path.c_str(), FILE_READ);
        const bool sourceValid = source && !source.isDirectory();
        const size_t sourceSize = sourceValid ? source.size() : 0;
        if (source) source.close();
        storageManager_.unlock();
        ok = sourceValid &&
            entry.m_uncomp_size == static_cast<mz_uint64>(sourceSize);
    }
    if (ok) ok = mz_zip_validate_archive(&archive, 0);
    if (initialized) mz_zip_reader_end(&archive);
    storageManager_.lock();
    input.close();
    storageManager_.unlock();
    return ok;
}

int ArchiveManager::deleteVerifiedWeeklySources() {
    int retained = 0;
    storageManager_.lock();
    for (const String& path : targetFiles_) {
        const bool removed = SD.exists(path.c_str()) &&
            SD.remove(path.c_str()) && !SD.exists(path.c_str());
        if (!removed) ++retained;
    }
    storageManager_.unlock();
    if (retained != 0) {
        Logger::warn("ArchiveMgr",
            "%d source files retained after verified weekly archive",
            retained);
    }
    return retained;
}

void ArchiveManager::cleanupTmpZip() {
    String tmpZipName = outputZipName_;
    tmpZipName.replace(".zip", ".tmp.zip");
    storageManager_.lock();
    if (SD.exists(tmpZipName.c_str())) SD.remove(tmpZipName.c_str());
    storageManager_.unlock();
}

} // namespace services
