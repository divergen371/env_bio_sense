#include "services/web_server_service.h"
#include "config/secrets.h"
#include "generated/web_assets.h"
#include "hal/clock.h"
#include "hal/i2c_bus.h"
#include "services/logger.h"
#include "services/sensor_manager.h"
#include "services/weather_service.h"
#include "services/web_security.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <SD.h>
#include <algorithm>
#include <cmath>
#include <esp_system.h>
#include <memory>
#include <vector>

extern services::WeatherService weatherService;
extern services::SensorManager sensorManager;

namespace services {
namespace {

constexpr const char* AUTH_REALM = "Env Bio Sense";
constexpr size_t MAX_LISTED_FILES = 512;
constexpr size_t MAX_BULK_FILES = 64;

struct ListedFile {
    String path;
    String name;
    String type;
    size_t size {};
    time_t modified {};
    bool current {false};
    bool activePpg {false};
    bool archiveBusy {false};
};

struct DownloadState {
    File file;
    storage::StorageManager* storage {};
    size_t size {};
    bool closed {false};

    void close() {
        if (closed || storage == nullptr) return;
        storage->lock();
        if (file) file.close();
        storage->unlock();
        closed = true;
    }

    ~DownloadState() { close(); }
};

const char* timeSourceName(core::TimeSource source) {
    switch (source) {
        case core::TimeSource::Unset: return "UNSET";
        case core::TimeSource::Manual: return "MANUAL";
        case core::TimeSource::Ntp: return "NTP";
        case core::TimeSource::Gnss: return "GNSS";
        case core::TimeSource::Holdover: return "HOLDOVER";
    }
    return "UNKNOWN";
}

const char* ppgStateName(storage::PpgJournalState state) {
    switch (state) {
        case storage::PpgJournalState::Empty: return "IDLE";
        case storage::PpgJournalState::Preparing: return "PREPARING";
        case storage::PpgJournalState::Recording: return "RECORDING";
        case storage::PpgJournalState::Finalizing: return "FINALIZING";
        case storage::PpgJournalState::Committed: return "COMPLETED";
        case storage::PpgJournalState::RecoveryPending: return "RECOVERY_PENDING";
        case storage::PpgJournalState::CommittedPartial: return "RECOVERED_PARTIAL";
        case storage::PpgJournalState::RecoveryFailed: return "RECOVERY_FAILED";
        case storage::PpgJournalState::Aborted: return "ABORTED";
        case storage::PpgJournalState::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

bool ppgPathIsActive(const PpgSessionStatus& status, const char* path) {
    const bool active = status.state == storage::PpgJournalState::Preparing ||
        status.state == storage::PpgJournalState::Recording ||
        status.state == storage::PpgJournalState::Finalizing ||
        status.state == storage::PpgJournalState::RecoveryPending;
    return active && web_security::isPathInside(path, status.directory);
}

String baseName(const String& path) {
    const int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

String classifyFile(const String& path, const String& name) {
    if (path.endsWith(".zip")) return "archive";
    if (path.startsWith("/data/ppg/")) return "ppg";
    if (name.startsWith("log_") && name.endsWith(".csv")) return "log";
    return "diagnostic";
}

void addSecurityHeaders(AsyncWebServerResponse* response) {
    if (response == nullptr) return;
    response->addHeader("Cache-Control", "no-store, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("X-Content-Type-Options", "nosniff");
    response->addHeader("X-Frame-Options", "DENY");
    response->addHeader("Referrer-Policy", "no-referrer");
    response->addHeader("Content-Security-Policy",
        "default-src 'self'; script-src 'self'; style-src 'self'; "
        "img-src 'self' data:; connect-src 'self'; object-src 'none'; "
        "base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
}

void sendJsonError(AsyncWebServerRequest* request, int status,
                   const char* error) {
    JsonDocument document;
    document["status"] = "rejected";
    document["error"] = error;
    String body;
    serializeJson(document, body);
    AsyncWebServerResponse* response = request->beginResponse(
        status, "application/json", body);
    addSecurityHeaders(response);
    request->send(response);
}

void sendJsonResponse(AsyncWebServerRequest* request, int status,
                      const String& body) {
    AsyncWebServerResponse* response = request->beginResponse(
        status, "application/json", body);
    addSecurityHeaders(response);
    request->send(response);
}

void sendAsset(AsyncWebServerRequest* request, const char* contentType,
               const uint8_t* data, size_t size) {
    AsyncWebServerResponse* response = request->beginResponse(
        200, contentType, data, size);
    response->addHeader("Content-Encoding", "gzip");
    addSecurityHeaders(response);
    request->send(response);
}

void collectVisibleFiles(File& directory, std::vector<ListedFile>& output,
                         bool& truncated, uint8_t depth) {
    if (!directory || depth > 5 || output.size() >= MAX_LISTED_FILES) {
        truncated = output.size() >= MAX_LISTED_FILES;
        return;
    }
    File child = directory.openNextFile();
    while (child) {
        if (child.isDirectory()) {
            collectVisibleFiles(child, output, truncated, depth + 1);
        } else {
            const String rawPath = child.path();
            char normalized[web_security::MAX_DATA_PATH + 1] {};
            if (web_security::normalizeVisibleDataPath(
                    rawPath.c_str(), normalized, sizeof(normalized))) {
                ListedFile item;
                item.path = normalized;
                item.name = baseName(item.path);
                item.type = classifyFile(item.path, item.name);
                item.size = child.size();
                item.modified = child.getLastWrite();
                output.push_back(item);
                if (output.size() >= MAX_LISTED_FILES) truncated = true;
            }
        }
        child.close();
        if (truncated) return;
        child = directory.openNextFile();
    }
}

bool normalizeRequestPath(const String& requested, String& normalized) {
    char output[web_security::MAX_DATA_PATH + 1] {};
    if (!web_security::normalizeVisibleDataPath(
            requested.c_str(), output, sizeof(output))) return false;
    normalized = output;
    return true;
}

String contentTypeFor(const String& path) {
    if (path.endsWith(".csv")) return "text/csv; charset=utf-8";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".zip")) return "application/zip";
    return "application/octet-stream";
}

long queryLong(AsyncWebServerRequest* request, const char* name,
               long fallback, long minimum, long maximum) {
    if (!request->hasParam(name)) return fallback;
    const long value = request->getParam(name)->value().toInt();
    return value < minimum || value > maximum ? fallback : value;
}

} // namespace

WebServerService::WebServerService(
        storage::StorageManager& storageManager,
        ArchiveManager& archiveManager,
        LocationService& locationService,
        PpgSessionManager& ppgSessionManager)
    : storageManager_(storageManager),
      archiveManager_(archiveManager),
      locationService_(locationService),
      ppgSessionManager_(ppgSessionManager) {
    server_.reset(new AsyncWebServer(80));
}

bool WebServerService::authorize(AsyncWebServerRequest* request) const {
    if (request->authenticate(
            WEB_ADMIN_USER, WEB_ADMIN_PASSWORD, AUTH_REALM, false)) {
        return true;
    }
    request->requestAuthentication(AUTH_REALM, true);
    return false;
}

bool WebServerService::authorizeMutation(
        AsyncWebServerRequest* request) const {
    if (!authorize(request)) return false;
    if (!request->hasHeader("X-CSRF-Token") ||
        !web_security::constantTimeEquals(
            request->getHeader("X-CSRF-Token")->value().c_str(),
            csrfToken_)) {
        sendJsonError(request, 403, "CSRF_TOKEN_REQUIRED");
        return false;
    }
    return true;
}

void WebServerService::begin() {
    const uint32_t randomWords[4] = {
        esp_random(), esp_random(), esp_random(), esp_random()};
    std::snprintf(csrfToken_, sizeof(csrfToken_),
        "%08lX%08lX%08lX%08lX",
        static_cast<unsigned long>(randomWords[0]),
        static_cast<unsigned long>(randomWords[1]),
        static_cast<unsigned long>(randomWords[2]),
        static_cast<unsigned long>(randomWords[3]));
    setupRoutes();
    server_->begin();
    Logger::info("WebServer", "Authenticated server started on port 80");
}

void WebServerService::setupRoutes() {
    server_->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!authorize(request)) return;
        sendAsset(request, "text/html; charset=utf-8",
                  web_assets::INDEX_HTML_GZ,
                  web_assets::INDEX_HTML_GZ_SIZE);
    });
    server_->on("/app.css", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            sendAsset(request, "text/css; charset=utf-8",
                      web_assets::APP_CSS_GZ, web_assets::APP_CSS_GZ_SIZE);
        });
    server_->on("/app.js", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            sendAsset(request, "application/javascript; charset=utf-8",
                      web_assets::APP_JS_GZ, web_assets::APP_JS_GZ_SIZE);
        });

    server_->on("/api/session", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            const core::TimeSnapshot clock = hal::Clock::snapshot();
            JsonDocument document;
            document["csrf_token"] = csrfToken_;
            document["clock_valid"] = clock.utcValid;
            document["clock_source"] = timeSourceName(clock.source);
            String body;
            serializeJson(document, body);
            AsyncWebServerResponse* response = request->beginResponse(
                200, "application/json", body);
            addSecurityHeaders(response);
            request->send(response);
        });

    server_->on("/api/status", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            const storage::FramWalStats wal = storageManager_.getWalStats();
            const hal::I2cDiagnosticCounters i2c =
                hal::I2cBus::diagnosticTotals();
            const core::TimeSnapshot clock = hal::Clock::snapshot();
            JsonDocument document;
            document["pending"] = storageManager_.getPendingCount();
            document["max"] = storageManager_.getMaxRecords();
            document["sd_available"] = storageManager_.isSdAvailable();
            document["fram_read_only"] = storageManager_.isFramReadOnly();
            document["dropped_records"] = wal.droppedRecords;
            document["high_water_records"] = wal.highWaterRecords;
            document["event_count"] = storageManager_.getEventCount();
            document["event_max"] = storageManager_.getMaxEventRecords();
            document["i2c_lock_timeouts"] = i2c.lockTimeouts;
            document["i2c_communication_errors"] = i2c.communicationErrors;
            document["clock_valid"] = clock.utcValid;
            document["clock_source"] = timeSourceName(clock.source);
            document["clock_disciplined"] = clock.disciplined;
            String body;
            serializeJson(document, body);
            sendJsonResponse(request, 200, body);
        });

    server_->on("/api/location", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            const core::DeviceLocation location =
                locationService_.current(millis());
            JsonDocument document;
            document["valid"] = location.valid;
            document["source"] = core::locationSourceName(location.source);
            if (location.valid) {
                document["latitudeDeg"] = location.latitudeDeg;
                document["longitudeDeg"] = location.longitudeDeg;
                if (location.ageMs == UINT32_MAX) {
                    document["ageMs"] = nullptr;
                } else {
                    document["ageMs"] = location.ageMs;
                }
                if (location.source == core::LocationSource::GnssLive ||
                    location.source == core::LocationSource::GnssLastKnown) {
                    document["satellites"] = location.satellites;
                    if (std::isfinite(location.hdop)) {
                        document["hdop"] = location.hdop;
                    }
                }
            }
            String body;
            serializeJson(document, body);
            sendJsonResponse(request, 200, body);
        });

    server_->on("/api/weather", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            const AmedasStatus status = weatherService.status(millis());
            const bool hasPressure =
                (status.state == core::PressureFieldState::Valid ||
                 status.state == core::PressureFieldState::LastKnown) &&
                std::isfinite(status.interpolatedSeaLevelPressureHpa);
            JsonDocument document;
            document["state"] = core::pressureFieldStateName(status.state);
            document["has_pressure_field"] = hasPressure;
            if (hasPressure) {
                document["pressure_hpa"] =
                    status.interpolatedSeaLevelPressureHpa;
            }
            document["last_fetch_succeeded"] = status.lastFetchSucceeded;
            if (status.observationAgeMs == UINT32_MAX) {
                document["observation_age_ms"] = nullptr;
            } else {
                document["observation_age_ms"] = status.observationAgeMs;
            }
            document["cached_station_count"] = status.cachedStations;
            document["used_station_count"] = status.usedStations;
            JsonArray stations = document["stations"].to<JsonArray>();
            for (uint8_t i = 0; i < status.cachedStations && i < 5; ++i) {
                JsonObject station = stations.add<JsonObject>();
                station["id"] = status.stations[i].id;
                station["distance_km"] = status.stations[i].distanceKm;
                station["used"] = status.stations[i].used;
                if (std::isfinite(status.stations[i].seaLevelPressureHpa)) {
                    station["pressure_hpa"] =
                        status.stations[i].seaLevelPressureHpa;
                }
                if (status.stations[i].qualityCode == UINT8_MAX) {
                    station["quality_code"] = nullptr;
                } else {
                    station["quality_code"] =
                        status.stations[i].qualityCode;
                }
            }
            String body;
            serializeJson(document, body);
            sendJsonResponse(request, 200, body);
        });

    server_->on("/api/altitude", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            const core::SensorSnapshot snapshot = sensorManager.snapshot();
            const core::AltitudeTelemetry& altitude =
                snapshot.telemetry.altitude;
            float offset = NAN;
            float temperature = NAN;
            float pressure = NAN;
            uint32_t epoch = 0;
            const bool calibrated = storageManager_.getBmp581Calibration(
                offset, epoch, temperature, pressure);
            JsonDocument document;
            document["calibration_state"] = sensorManager.isBmp581Calibrating()
                ? "CALIBRATING" : (calibrated ? "CALIBRATED" : "UNCALIBRATED");
            document["altitude_valid"] = snapshot.environment.altitudeValid;
            if (snapshot.environment.altitudeValid) {
                document["raw_altitude_m"] = altitude.rawAltitudeM;
                document["display_altitude_m"] = altitude.displayAltitudeM;
            }
            document["pressure_field_state"] =
                core::pressureFieldStateName(altitude.pressureState);
            document["pressure_field_source"] =
                core::pressureReferenceSourceName(altitude.pressureSource);
            if (std::isfinite(altitude.calculationTemperatureC)) {
                document["calculation_temperature_c"] =
                    altitude.calculationTemperatureC;
                document["calculation_temperature_source"] =
                    altitude.externalTemperatureSource ? "SHT45" : "BMP581";
            }
            if (calibrated) {
                document["calibration_offset_hpa"] = offset;
                document["calibration_epoch"] = epoch;
            }
            String body;
            serializeJson(document, body);
            sendJsonResponse(request, 200, body);
        });

    server_->on("/api/ppg", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            const PpgSessionStatus status = ppgSessionManager_.status();
            JsonDocument document;
            document["state"] = ppgStateName(status.state);
            document["error"] = ppgSessionErrorName(status.error);
            document["session_id"] = status.sessionId;
            document["directory"] = status.directory;
            document["block_count"] = status.blockCount;
            document["stored_samples"] = status.storedSamples;
            document["dropped_samples"] = status.droppedSamples;
            document["fifo_overflows"] = status.fifoOverflows;
            document["ring_high_water_samples"] = status.ringHighWaterSamples;
            document["max_sd_write_us"] = status.maxSdWriteUs;
            document["buffer_memory"] = status.usingPsram ? "PSRAM" : "INTERNAL";
            String body;
            serializeJson(document, body);
            sendJsonResponse(request, 200, body);
        });

    server_->on("/api/files", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            std::vector<ListedFile> files;
            files.reserve(96);
            bool truncated = false;
            storageManager_.lock();
            File root = SD.open("/");
            if (root) collectVisibleFiles(root, files, truncated, 0);
            if (root) root.close();
            storageManager_.unlock();

            const String currentLog = storageManager_.getCurrentFilename();
            const PpgSessionStatus ppg = ppgSessionManager_.status();
            for (ListedFile& file : files) {
                file.current = file.path == currentLog;
                file.activePpg = ppgPathIsActive(ppg, file.path.c_str());
                file.archiveBusy = archiveManager_.isFileBusy(file.path);
            }

            String query = request->hasParam("q")
                ? request->getParam("q")->value() : "";
            String type = request->hasParam("type")
                ? request->getParam("type")->value() : "all";
            query.toLowerCase();
            std::vector<ListedFile> filtered;
            filtered.reserve(files.size());
            for (const ListedFile& file : files) {
                String candidate = file.path;
                candidate.toLowerCase();
                if ((!query.isEmpty() && candidate.indexOf(query) < 0) ||
                    (type != "all" && file.type != type)) continue;
                filtered.push_back(file);
            }
            const String sort = request->hasParam("sort")
                ? request->getParam("sort")->value() : "name";
            const bool ascending = request->hasParam("order") &&
                request->getParam("order")->value() == "asc";
            std::sort(filtered.begin(), filtered.end(),
                [sort, ascending](const ListedFile& a, const ListedFile& b) {
                    int comparison = 0;
                    if (sort == "size") {
                        comparison = a.size == b.size ? 0 :
                            (a.size < b.size ? -1 : 1);
                    } else {
                        comparison = a.path.compareTo(b.path);
                    }
                    return ascending ? comparison < 0 : comparison > 0;
                });

            const size_t limit = static_cast<size_t>(
                queryLong(request, "limit", 50, 10, 100));
            const size_t total = filtered.size();
            const size_t pageCount = std::max<size_t>(
                1, (total + limit - 1) / limit);
            const size_t requestedPage = static_cast<size_t>(
                queryLong(request, "page", 1, 1, 10000));
            const size_t page = std::min(requestedPage, pageCount);
            const size_t first = std::min(total, (page - 1) * limit);
            const size_t last = std::min(total, first + limit);

            AsyncResponseStream* response =
                request->beginResponseStream("application/json", 12288);
            response->printf(
                "{\"page\":%u,\"page_count\":%u,\"total\":%u,\"truncated\":%s,\"files\":[",
                static_cast<unsigned>(page),
                static_cast<unsigned>(pageCount),
                static_cast<unsigned>(total), truncated ? "true" : "false");
            for (size_t i = first; i < last; ++i) {
                const ListedFile& file = filtered[i];
                if (i != first) response->print(',');
                const bool locked = file.current || file.activePpg ||
                    file.archiveBusy;
                const bool selectable = !locked;
                const bool downloadable = !file.activePpg;
                response->printf(
                    "{\"path\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"size\":%u,\"modified\":%lld,\"locked\":%s,\"selectable\":%s,\"downloadable\":%s}",
                    file.path.c_str(), file.name.c_str(), file.type.c_str(),
                    static_cast<unsigned>(file.size),
                    static_cast<long long>(file.modified),
                    locked ? "true" : "false",
                    selectable ? "true" : "false",
                    downloadable ? "true" : "false");
            }
            response->print("]}");
            addSecurityHeaders(response);
            request->send(response);
        });

    server_->on("/download", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            if (!request->hasParam("file")) {
                sendJsonError(request, 400, "MISSING_FILE");
                return;
            }
            String path;
            if (!normalizeRequestPath(
                    request->getParam("file")->value(), path)) {
                sendJsonError(request, 400, "INVALID_FILE_PATH");
                return;
            }
            const PpgSessionStatus ppg = ppgSessionManager_.status();
            if (ppgPathIsActive(ppg, path.c_str())) {
                sendJsonError(request, 423, "ACTIVE_PPG_FILE_LOCKED");
                return;
            }
            if (path == storageManager_.getCurrentFilename()) {
                // Wi-Fi mode pauses normal WAL flushes. Force one verified
                // boundary before opening, after which this file is stable.
                storageManager_.forceFlush();
            }
            std::shared_ptr<DownloadState> state(new DownloadState());
            state->storage = &storageManager_;
            storageManager_.lock();
            state->file = SD.open(path.c_str(), FILE_READ);
            const bool valid = state->file && !state->file.isDirectory();
            state->size = valid ? state->file.size() : 0;
            storageManager_.unlock();
            if (!valid) {
                state->close();
                sendJsonError(request, 404, "FILE_NOT_FOUND");
                return;
            }
            AsyncWebServerResponse* response = request->beginResponse(
                contentTypeFor(path), state->size,
                [state](uint8_t* buffer, size_t maxLength,
                        size_t index) -> size_t {
                    if (index >= state->size || state->closed) {
                        state->close();
                        return 0;
                    }
                    const size_t wanted = std::min(
                        maxLength, state->size - index);
                    state->storage->lock();
                    bool positioned = state->file.position() == index;
                    if (!positioned) positioned = state->file.seek(index);
                    const size_t actual = positioned
                        ? state->file.read(buffer, wanted) : 0;
                    if (index + actual >= state->size) {
                        state->file.close();
                        state->closed = true;
                    }
                    state->storage->unlock();
                    return actual;
                });
            const String disposition = "attachment; filename=\"" +
                baseName(path) + "\"";
            response->addHeader("Content-Disposition", disposition);
            response->addHeader("Connection", "close");
            addSecurityHeaders(response);
            request->send(response);
        });

    server_->on("/api/i2c", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            AsyncResponseStream* response =
                request->beginResponseStream("application/json");
            const hal::I2cDiagnosticCounters totals =
                hal::I2cBus::diagnosticTotals();
            response->printf(
                "{\"lock_timeouts\":%lu,\"communication_errors\":%lu,\"breakdown\":[",
                static_cast<unsigned long>(totals.lockTimeouts),
                static_cast<unsigned long>(totals.communicationErrors));
            bool first = true;
            for (uint8_t deviceIndex = 0;
                 deviceIndex < static_cast<uint8_t>(hal::I2cDevice::Count);
                 ++deviceIndex) {
                for (uint8_t operationIndex = 0;
                     operationIndex < static_cast<uint8_t>(hal::I2cOperation::Count);
                     ++operationIndex) {
                    const auto device =
                        static_cast<hal::I2cDevice>(deviceIndex);
                    const auto operation =
                        static_cast<hal::I2cOperation>(operationIndex);
                    const hal::I2cDiagnosticCounters counters =
                        hal::I2cBus::diagnostics(device, operation);
                    if (counters.lockTimeouts == 0 &&
                        counters.communicationErrors == 0) continue;
                    if (!first) response->print(',');
                    first = false;
                    response->printf(
                        "{\"device\":\"%s\",\"operation\":\"%s\",\"lock_timeouts\":%lu,\"communication_errors\":%lu}",
                        hal::i2cDeviceName(device),
                        hal::i2cOperationName(operation),
                        static_cast<unsigned long>(counters.lockTimeouts),
                        static_cast<unsigned long>(counters.communicationErrors));
                }
            }
            response->print("]}");
            addSecurityHeaders(response);
            request->send(response);
        });

    server_->on("/api/events", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            constexpr size_t MAX_EVENTS = 64;
            const size_t requested = static_cast<size_t>(
                queryLong(request, "limit", MAX_EVENTS, 1, MAX_EVENTS));
            storage::EventRecord events[MAX_EVENTS] {};
            const size_t count =
                storageManager_.readRecentEvents(events, requested);
            AsyncResponseStream* response =
                request->beginResponseStream("application/json", 8192);
            response->printf(
                "{\"retained\":%u,\"returned\":%u,\"events\":[",
                storageManager_.getEventCount(),
                static_cast<unsigned>(count));
            for (size_t i = 0; i < count; ++i) {
                if (i != 0) response->print(',');
                const storage::EventCode code =
                    static_cast<storage::EventCode>(events[i].eventCode);
                response->printf(
                    "{\"sequence\":%lu,\"uptime_ms\":%lu,\"code\":%u,\"name\":\"%s\",\"detail\":%ld}",
                    static_cast<unsigned long>(events[i].header.sequence),
                    static_cast<unsigned long>(events[i].uptimeMs),
                    events[i].eventCode, storage::eventCodeName(code),
                    static_cast<long>(events[i].detail));
            }
            response->print("]}");
            response->addHeader("Content-Disposition",
                                "attachment; filename=\"scd41_events.json\"");
            addSecurityHeaders(response);
            request->send(response);
        });

    auto* timeHandler = new AsyncCallbackJsonWebHandler(
        "/api/time", [this](AsyncWebServerRequest* request,
                             JsonVariant& json) {
            if (!authorizeMutation(request)) return;
            if (!json.is<JsonObject>() ||
                !json["epoch"].is<int64_t>()) {
                sendJsonError(request, 400, "INVALID_EPOCH");
                return;
            }
            const int64_t epoch = json["epoch"].as<int64_t>();
            if (epoch < 1704067200LL || epoch > 4102444800LL) {
                sendJsonError(request, 400, "EPOCH_OUT_OF_RANGE");
                return;
            }
            if (hal::Clock::isDisciplined()) {
                sendJsonError(request, 409, "DISCIPLINED_CLOCK_ACTIVE");
                return;
            }
            hal::Clock::setEpoch(static_cast<time_t>(epoch));
            sendJsonResponse(request, 200, "{\"status\":\"ok\"}");
        });
    timeHandler->setMethod(HTTP_POST);
    timeHandler->setMaxContentLength(128);
    server_->addHandler(timeHandler);

    server_->on("/api/flush", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            if (!authorizeMutation(request)) return;
            storageManager_.forceFlush();
            sendJsonResponse(request, 200, "{\"status\":\"ok\"}");
        });

    auto* deleteHandler = new AsyncCallbackJsonWebHandler(
        "/api/files/delete",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!authorizeMutation(request)) return;
            if (!json.is<JsonObject>() ||
                !json["confirmation"].is<const char*>() ||
                !web_security::constantTimeEquals(
                    json["confirmation"].as<const char*>(),
                    "DELETE_SELECTED") ||
                !json["files"].is<JsonArray>()) {
                sendJsonError(request, 400, "INVALID_DELETE_REQUEST");
                return;
            }
            const JsonArray files = json["files"].as<JsonArray>();
            if (files.size() == 0 || files.size() > MAX_BULK_FILES) {
                sendJsonError(request, 400, "INVALID_FILE_COUNT");
                return;
            }
            JsonDocument result;
            JsonArray deleted = result["deleted"].to<JsonArray>();
            JsonArray rejected = result["rejected"].to<JsonArray>();
            const String currentLog = storageManager_.getCurrentFilename();
            const PpgSessionStatus ppg = ppgSessionManager_.status();
            for (JsonVariant value : files) {
                String path;
                if (!value.is<const char*>() ||
                    !normalizeRequestPath(value.as<String>(), path) ||
                    path == currentLog ||
                    ppgPathIsActive(ppg, path.c_str()) ||
                    archiveManager_.isFileBusy(path)) {
                    rejected.add(value.as<String>());
                    continue;
                }
                storageManager_.lock();
                File file = SD.open(path.c_str(), FILE_READ);
                const bool regular = file && !file.isDirectory();
                if (file) file.close();
                const bool removed = regular && SD.remove(path.c_str()) &&
                    !SD.exists(path.c_str());
                storageManager_.unlock();
                if (removed) {
                    deleted.add(path);
                    Logger::warn("WebServer", "File deleted: %s", path.c_str());
                } else {
                    rejected.add(path);
                }
            }
            String body;
            serializeJson(result, body);
            sendJsonResponse(request, 200, body);
        });
    deleteHandler->setMethod(HTTP_POST);
    deleteHandler->setMaxContentLength(8192);
    server_->addHandler(deleteHandler);

    auto* archiveHandler = new AsyncCallbackJsonWebHandler(
        "/api/archive/manual",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!authorizeMutation(request)) return;
            if (!json.is<JsonObject>() ||
                !json["files"].is<JsonArray>()) {
                sendJsonError(request, 400, "INVALID_ARCHIVE_REQUEST");
                return;
            }
            const JsonArray input = json["files"].as<JsonArray>();
            if (input.size() == 0 || input.size() > MAX_BULK_FILES) {
                sendJsonError(request, 400, "INVALID_FILE_COUNT");
                return;
            }
            const String currentLog = storageManager_.getCurrentFilename();
            const PpgSessionStatus ppg = ppgSessionManager_.status();
            std::vector<String> paths;
            paths.reserve(input.size());
            for (JsonVariant value : input) {
                String path;
                if (!value.is<const char*>() ||
                    !normalizeRequestPath(value.as<String>(), path) ||
                    path == currentLog ||
                    ppgPathIsActive(ppg, path.c_str())) {
                    sendJsonError(request, 409,
                                  "FILE_ACTIVE_OR_NOT_ARCHIVABLE");
                    return;
                }
                storageManager_.lock();
                File selected = SD.open(path.c_str(), FILE_READ);
                const bool regular = selected && !selected.isDirectory();
                if (selected) selected.close();
                storageManager_.unlock();
                if (!regular) {
                    sendJsonError(request, 404, "FILE_NOT_FOUND");
                    return;
                }
                paths.push_back(path);
            }
            if (!archiveManager_.startManualArchive(paths)) {
                sendJsonError(request, 409,
                              "ARCHIVE_BUSY_OR_INVALID_SELECTION");
                return;
            }
            sendJsonResponse(request, 202,
                             "{\"status\":\"scheduled\"}");
        });
    archiveHandler->setMethod(HTTP_POST);
    archiveHandler->setMaxContentLength(8192);
    server_->addHandler(archiveHandler);

    server_->on("/api/archive/cancel", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            if (!authorizeMutation(request)) return;
            archiveManager_.cancelArchive();
            sendJsonResponse(request, 202,
                             "{\"status\":\"cancellation_requested\"}");
        });

    server_->on("/api/archive/status", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!authorize(request)) return;
            const ArchiveStatus status = archiveManager_.getStatus();
            JsonDocument document;
            document["state"] = static_cast<int>(status.state);
            document["currentFile"] = status.currentFile;
            document["processedFiles"] = status.processedFiles;
            document["totalFiles"] = status.totalFiles;
            document["processedBytes"] = status.processedBytes;
            document["totalBytes"] = status.totalBytes;
            document["progressPercent"] = status.progressPercent;
            document["message"] = status.message;
            document["outputFile"] = status.outputFile;
            document["verified"] = status.verified;
            document["retainedOriginals"] = status.retainedOriginals;
            String body;
            serializeJson(document, body);
            sendJsonResponse(request, 200, body);
        });

    auto* scdCalibrationHandler = new AsyncCallbackJsonWebHandler(
        "/api/scd41/calibrate",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!authorizeMutation(request)) return;
            if (!json.is<JsonObject>() ||
                !json["reference_ppm"].is<int32_t>() ||
                !json["confirm_external_reference"].is<bool>() ||
                !json["confirm_external_reference"].as<bool>()) {
                sendJsonError(request, 400,
                              "EXTERNAL_REFERENCE_NOT_CONFIRMED");
                return;
            }
            const int32_t reference = json["reference_ppm"].as<int32_t>();
            if (reference < 400 || reference > 5000) {
                sendJsonError(request, 400, "REFERENCE_OUT_OF_RANGE");
                return;
            }
            drivers::sensors::Scd41FrcResult result;
            const bool success = sensorManager.calibrateScd41(
                static_cast<uint16_t>(reference), result);
            JsonDocument responseDocument;
            responseDocument["status"] = success ? "ok" : "failed";
            responseDocument["reference_ppm"] = result.referencePpm;
            responseDocument["pre_co2_ppm"] = result.preCalibrationCo2Ppm;
            responseDocument["correction_ppm"] = result.correctionPpm;
            responseDocument["raw_word"] = result.rawWord;
            responseDocument["ambient_pressure_hpa"] =
                result.ambientPressureHpa;
            responseDocument["measurement_uptime_ms"] =
                result.measurementUptimeMs;
            responseDocument["restart_success"] = result.restartSuccess;
            if (!success) {
                responseDocument["error"] = result.errorMessage != nullptr
                    ? result.errorMessage : "FRC_FAILED";
            }
            String body;
            serializeJson(responseDocument, body);
            sendJsonResponse(request, success ? 200 : 409, body);
        });
    scdCalibrationHandler->setMethod(HTTP_POST);
    scdCalibrationHandler->setMaxContentLength(256);
    server_->addHandler(scdCalibrationHandler);

    auto* scdResetHandler = new AsyncCallbackJsonWebHandler(
        "/api/scd41/factory_reset",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!authorizeMutation(request)) return;
            if (!json.is<JsonObject>() ||
                !json["confirmation"].is<const char*>() ||
                !web_security::constantTimeEquals(
                    json["confirmation"].as<const char*>(),
                    "RESET_SCD41")) {
                sendJsonError(request, 400, "CONFIRMATION_REQUIRED");
                return;
            }
            const bool success = sensorManager.factoryResetScd41();
            sendJsonResponse(request, success ? 200 : 500,
                success ? "{\"status\":\"ok\"}"
                        : "{\"status\":\"failed\",\"error\":\"FACTORY_RESET_FAILED\"}");
        });
    scdResetHandler->setMethod(HTTP_POST);
    scdResetHandler->setMaxContentLength(128);
    server_->addHandler(scdResetHandler);

    auto* bmpCalibrationHandler = new AsyncCallbackJsonWebHandler(
        "/api/bmp581/calibrate",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            if (!authorizeMutation(request)) return;
            if (!json.is<JsonObject>() ||
                !json["reference_altitude_m"].is<float>()) {
                sendJsonError(request, 400,
                              "INVALID_REFERENCE_ALTITUDE");
                return;
            }
            const float reference =
                json["reference_altitude_m"].as<float>();
            if (!utils::bmp581_calibration::validReferenceAltitude(reference)) {
                sendJsonError(request, 400, "REFERENCE_MUST_BE_13_6_M");
                return;
            }
            if (!sensorManager.startBmp581Calibration(reference)) {
                sendJsonError(request, 409,
                    "CALIBRATION_BUSY_OR_PRESSURE_FIELD_NOT_VALID");
                return;
            }
            sendJsonResponse(request, 202,
                "{\"status\":\"started\",\"message\":\"Calibration started\",\"reference_altitude_m\":13.6}");
        });
    bmpCalibrationHandler->setMethod(HTTP_POST);
    bmpCalibrationHandler->setMaxContentLength(128);
    server_->addHandler(bmpCalibrationHandler);

    server_->onNotFound([this](AsyncWebServerRequest* request) {
        if (!authorize(request)) return;
        sendJsonError(request, 404, "NOT_FOUND");
    });
}

} // namespace services
