#include "services/weather_service.h"
#include "services/logger.h"
#include "config/secrets.h"
#include "config/jma_tls.h"
#include "hal/clock.h"
#include "utils/amedas_policy.h"
#include "utils/location_policy.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace services {
namespace {

constexpr uint32_t FETCH_INTERVAL_MS = 15u * 60u * 1000u;
constexpr uint32_t RETRY_INTERVAL_MS = 5u * 60u * 1000u;
constexpr uint32_t CLOCK_RETRY_INTERVAL_MS = 30u * 1000u;

bool parseFloatStrict(const String& text, float& value) {
    errno = 0;
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parseQualityStrict(const String& text, uint8_t& value) {
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed < 0 || parsed > UINT8_MAX) {
        return false;
    }
    value = static_cast<uint8_t>(parsed);
    return true;
}

bool parseNormalPressure(const String& objectText, float& pressureHpa,
                         uint8_t& qualityCode) {
    const int field = objectText.indexOf("\"normalPressure\":[");
    if (field < 0) return false;
    const int valueStart = field + 18;
    const int comma = objectText.indexOf(',', valueStart);
    const int close = objectText.indexOf(']', comma + 1);
    if (comma < 0 || close < 0) return false;
    return parseFloatStrict(objectText.substring(valueStart, comma), pressureHpa) &&
           parseQualityStrict(objectText.substring(comma + 1, close), qualityCode);
}

bool appendFormat(char* buffer, size_t capacity, size_t& length,
                  const char* format, ...) {
    if (buffer == nullptr || format == nullptr || length >= capacity) {
        return false;
    }
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(
        buffer + length, capacity - length, format, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= capacity - length) {
        return false;
    }
    length += static_cast<size_t>(written);
    return true;
}

} // namespace

WeatherService::WeatherService(SensorManager& sensorManager,
                               WifiManager& wifiManager,
                               LocationService& locationService,
                               storage::StorageManager& storageManager)
    : sensorManager_(sensorManager),
      wifiManager_(wifiManager),
      locationService_(locationService),
      storageManager_(storageManager) {}

void WeatherService::begin() {
    nextFetchMs_ = 0;
    numCachedStations_ = 0;
    hasStationSelectionLocation_ = false;
    hasLastGoodField_ = false;
    lastGoodObservationCapturedMs_ = 0;
    publishStatus({});
}

void WeatherService::publishStatus(const AmedasStatus& status) {
    portENTER_CRITICAL(&statusMux_);
    status_ = status;
    portEXIT_CRITICAL(&statusMux_);
}

AmedasStatus WeatherService::status(uint32_t nowMs) const {
    portENTER_CRITICAL(&statusMux_);
    AmedasStatus copy = status_;
    const uint32_t capturedMs = lastGoodObservationCapturedMs_;
    const bool hasLastGood = hasLastGoodField_;
    portEXIT_CRITICAL(&statusMux_);
    copy.observationAgeMs = hasLastGood
        ? nowMs - capturedMs : UINT32_MAX;
    return copy;
}

void WeatherService::persistPressureFieldAttempt(
        const AmedasStatus& attempt, float previousPressureHpa,
        core::PressureFieldState resultingState, const char* result) {
    const core::TimeSnapshot clock = hal::Clock::snapshot();
    if (!clock.utcValid || !attempt.location.valid || result == nullptr) return;

    char line[768] {};
    size_t length = 0;
    bool ok = appendFormat(line, sizeof(line), length,
        "%lld,%lld,%s,%.7f,%.7f,",
        static_cast<long long>(clock.utcEpochUs / 1000LL),
        static_cast<long long>(attempt.observationUtcMs),
        core::locationSourceName(attempt.location.source),
        attempt.location.latitudeDeg, attempt.location.longitudeDeg);
    if (ok && std::isfinite(previousPressureHpa)) {
        ok = appendFormat(line, sizeof(line), length, "%.1f",
            previousPressureHpa);
    }
    ok = ok && appendFormat(line, sizeof(line), length, ",");
    if (ok && std::isfinite(attempt.interpolatedSeaLevelPressureHpa) &&
        attempt.interpolatedSeaLevelPressureHpa > 800.0f &&
        attempt.interpolatedSeaLevelPressureHpa < 1100.0f) {
        ok = appendFormat(line, sizeof(line), length, "%.1f",
            attempt.interpolatedSeaLevelPressureHpa);
    }
    ok = ok && appendFormat(line, sizeof(line), length,
        ",%s,%s,%u,%u",
        core::pressureFieldStateName(resultingState), result,
        attempt.cachedStations, attempt.usedStations);

    for (uint8_t i = 0; ok && i < 5u; ++i) {
        if (i >= attempt.cachedStations || attempt.stations[i].id[0] == '\0') {
            ok = appendFormat(line, sizeof(line), length, ",,,,,");
            continue;
        }
        ok = appendFormat(line, sizeof(line), length, ",%s,%.2f,",
            attempt.stations[i].id, attempt.stations[i].distanceKm);
        if (ok && std::isfinite(attempt.stations[i].seaLevelPressureHpa) &&
            attempt.stations[i].seaLevelPressureHpa > 800.0f &&
            attempt.stations[i].seaLevelPressureHpa < 1100.0f) {
            ok = appendFormat(line, sizeof(line), length, "%.1f",
                attempt.stations[i].seaLevelPressureHpa);
        }
        ok = ok && appendFormat(line, sizeof(line), length, ",");
        if (ok && attempt.stations[i].qualityCode != UINT8_MAX) {
            ok = appendFormat(line, sizeof(line), length, "%u",
                attempt.stations[i].qualityCode);
        }
        ok = ok && appendFormat(line, sizeof(line), length, ",%u",
            attempt.stations[i].used ? 1u : 0u);
    }
    if (!ok || !storageManager_.appendAmedasLogLine(line)) {
        Logger::error("Weather", "Failed to persist AMeDAS provenance log");
    }
}

void WeatherService::refreshPressureFieldState(uint32_t nowMs) {
    portENTER_CRITICAL(&statusMux_);
    const bool hasLastGood = hasLastGoodField_;
    const uint32_t capturedMs = lastGoodObservationCapturedMs_;
    const core::PressureFieldState previous = status_.state;
    portEXIT_CRITICAL(&statusMux_);
    if (!hasLastGood) return;

    const uint32_t ageMs = nowMs - capturedMs;
    core::PressureFieldState next = core::PressureFieldState::Invalid;
    if (ageMs <= utils::amedas_policy::OBSERVATION_MAX_AGE_MS) {
        next = core::PressureFieldState::Valid;
    } else if (ageMs <= utils::amedas_policy::LAST_KNOWN_MAX_AGE_MS) {
        next = core::PressureFieldState::LastKnown;
    }

    portENTER_CRITICAL(&statusMux_);
    status_.state = next;
    status_.observationAgeMs = ageMs;
    portEXIT_CRITICAL(&statusMux_);
    if (next != previous) {
        Logger::warn("Weather", "Pressure field state changed: %d -> %d age_ms=%lu",
            static_cast<int>(previous), static_cast<int>(next),
            static_cast<unsigned long>(ageMs));
        sensorManager_.setSeaLevelPressureState(next);
    }
}

void WeatherService::update(uint32_t nowMs) {
    refreshPressureFieldState(nowMs);

    const core::DeviceLocation location = locationService_.current(nowMs);
    if (location.valid && hasStationSelectionLocation_) {
        const double movedM = LocationService::distanceMeters(
            stationSelectionLatDeg_, stationSelectionLonDeg_,
            location.latitudeDeg, location.longitudeDeg);
        if (utils::location_policy::shouldReselectStations(movedM)) {
            Logger::info("Weather",
                "AMeDAS station cache invalidated: moved %.2f km",
                movedM / 1000.0);
            numCachedStations_ = 0;
            hasStationSelectionLocation_ = false;
            nextFetchMs_ = nowMs;
        }
    }

    if (wifiManager_.isOn()) return;
    if (static_cast<int32_t>(nowMs - nextFetchMs_) < 0) return;
    if (!location.valid) {
        Logger::warn("Weather", "AMeDAS fetch deferred: no valid device location");
        nextFetchMs_ = nowMs + RETRY_INTERVAL_MS;
        return;
    }
    if (!hal::Clock::isTimeSet()) {
        Logger::warn("Weather",
            "AMeDAS fetch deferred: UTC is required for TLS and observation-age validation");
        nextFetchMs_ = nowMs + CLOCK_RETRY_INTERVAL_MS;
        return;
    }

    if (fetchSeaLevelPressure(location, nowMs)) {
        nextFetchMs_ = nowMs + FETCH_INTERVAL_MS;
    } else {
        portENTER_CRITICAL(&statusMux_);
        status_.lastFetchSucceeded = false;
        portEXIT_CRITICAL(&statusMux_);
        nextFetchMs_ = nowMs + RETRY_INTERVAL_MS;
    }
}

void WeatherService::forceUpdate(float pressureHpa) {
    Logger::info("Weather",
        "Forcing Sea Level Pressure update from API: %.1f hPa", pressureHpa);
    sensorManager_.setSeaLevelPressure(
        pressureHpa, core::PressureFieldState::Valid,
        core::PressureReferenceSource::Manual, 0);
    nextFetchMs_ = millis() + FETCH_INTERVAL_MS;
}

bool WeatherService::fetchNearestStations(
        const core::DeviceLocation& location) {
    Logger::info("Weather", "Selecting AMeDAS stations at %s location",
        core::locationSourceName(location.source));

    WiFiClientSecure client;
    client.setCACert(config::JMA_GLOBALSIGN_ROOT_R46);
    HTTPClient http;
    if (!http.begin(client,
            "https://www.jma.go.jp/bosai/amedas/const/amedastable.json")) {
        return false;
    }
    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Logger::error("Weather", "Failed to get amedastable. HTTP: %d", httpCode);
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    numCachedStations_ = 0;
    int braceLevel = 0;
    String objectText;
    String currentStation;
    uint8_t buffer[128];
    int remaining = http.getSize();
    uint32_t lastDataMs = millis();
    bool insideRootKey = false;

    while ((http.connected() || stream->available()) &&
           millis() - lastDataMs < 15000u) {
        const size_t available = stream->available();
        if (available == 0) {
            delay(1);
            continue;
        }
        lastDataMs = millis();
        const int read = stream->readBytes(buffer,
            available > sizeof(buffer) ? sizeof(buffer) : available);
        for (int i = 0; i < read; ++i) {
            const char ch = static_cast<char>(buffer[i]);
            if (braceLevel == 1 && ch == '"' && !insideRootKey) {
                insideRootKey = true;
                currentStation = "";
            } else if (braceLevel == 1 && ch == '"' && insideRootKey) {
                insideRootKey = false;
            } else if (insideRootKey) {
                currentStation += ch;
            }
            if (ch == '{') ++braceLevel;
            if (ch == '}') --braceLevel;
            if (braceLevel >= 2) {
                objectText += ch;
            } else if (braceLevel == 1 && ch == '}' &&
                       objectText.length() > 0) {
                if (currentStation.length() == 5 &&
                    isDigit(currentStation[0])) {
                    const int typeIndex = objectText.indexOf("\"type\":\"");
                    const char type = typeIndex >= 0
                        ? objectText.charAt(typeIndex + 8) : '\0';
                    if (type == 'A' || type == 'B') {
                        const int latIndex = objectText.indexOf("\"lat\":[");
                        const int lonIndex = objectText.indexOf("\"lon\":[");
                        const int latEnd = objectText.indexOf(']', latIndex);
                        const int lonEnd = objectText.indexOf(']', lonIndex);
                        if (latIndex >= 0 && lonIndex >= 0 &&
                            latEnd > latIndex && lonEnd > lonIndex) {
                            const String latText = objectText.substring(
                                latIndex + 7, latEnd);
                            const String lonText = objectText.substring(
                                lonIndex + 7, lonEnd);
                            const int latComma = latText.indexOf(',');
                            const int lonComma = lonText.indexOf(',');
                            if (latComma > 0 && lonComma > 0) {
                                const double latitude =
                                    latText.substring(0, latComma).toDouble() +
                                    latText.substring(latComma + 1).toDouble() / 60.0;
                                const double longitude =
                                    lonText.substring(0, lonComma).toDouble() +
                                    lonText.substring(lonComma + 1).toDouble() / 60.0;
                                const double distanceM =
                                    LocationService::distanceMeters(
                                        location.latitudeDeg,
                                        location.longitudeDeg,
                                        latitude, longitude);
                                if (std::isfinite(distanceM)) {
                                    const float distanceKm =
                                        static_cast<float>(distanceM / 1000.0);
                                    if (numCachedStations_ < 5 ||
                                        distanceKm < nearestStations_[4].distanceKm) {
                                        int position = numCachedStations_ < 5
                                            ? numCachedStations_ : 4;
                                        while (position > 0 &&
                                            nearestStations_[position - 1].distanceKm >
                                                distanceKm) {
                                            if (position < 5) {
                                                nearestStations_[position] =
                                                    nearestStations_[position - 1];
                                            }
                                            --position;
                                        }
                                        if (position < 5) {
                                            std::strncpy(nearestStations_[position].id,
                                                currentStation.c_str(), 5);
                                            nearestStations_[position].id[5] = '\0';
                                            nearestStations_[position].distanceKm =
                                                distanceKm;
                                            if (numCachedStations_ < 5) {
                                                ++numCachedStations_;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                objectText = "";
                currentStation = "";
            }
        }
        if (remaining > 0) {
            remaining -= read;
            if (remaining <= 0) break;
        }
    }
    http.end();

    if (numCachedStations_ == 0) return false;
    stationSelectionLatDeg_ = location.latitudeDeg;
    stationSelectionLonDeg_ = location.longitudeDeg;
    stationSelectionSource_ = location.source;
    hasStationSelectionLocation_ = true;
    Logger::info("Weather", "Found %d pressure-capable AMeDAS stations",
        numCachedStations_);
    return true;
}

bool WeatherService::fetchLatestTime(String& mapTime,
                                     int64_t& observationUtcMs) {
    WiFiClientSecure client;
    client.setCACert(config::JMA_GLOBALSIGN_ROOT_R46);
    HTTPClient http;
    if (!http.begin(client,
            "https://www.jma.go.jp/bosai/amedas/data/latest_time.txt")) {
        return false;
    }
    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        return false;
    }
    String latest = http.getString();
    http.end();
    latest.trim();
    if (!utils::amedas_policy::parseJstIso8601(
            latest.c_str(), observationUtcMs)) {
        Logger::error("Weather", "Invalid AMeDAS latest_time format");
        return false;
    }
    const core::TimeSnapshot now = hal::Clock::snapshot();
    if (!now.utcValid) {
        Logger::warn("Weather", "Cannot validate AMeDAS age: system UTC unavailable");
        return false;
    }
    uint32_t observationAge = 0;
    if (!utils::amedas_policy::observationAgeMs(
            now.utcEpochUs / 1000LL, observationUtcMs, observationAge)) {
        Logger::error("Weather", "AMeDAS observation is stale or future-dated");
        return false;
    }
    mapTime = latest;
    mapTime.replace("-", "");
    mapTime.replace("T", "");
    mapTime.replace(":", "");
    if (mapTime.length() < 14) return false;
    mapTime = mapTime.substring(0, 14);
    return true;
}

bool WeatherService::fetchSeaLevelPressure(
        const core::DeviceLocation& location, uint32_t nowMs) {
    Logger::info("Weather", "Fetching Sea Level Pressure from JMA AMeDAS");
    const core::SensorSnapshot previousSnapshot = sensorManager_.snapshot();
    const core::PressureFieldState previousState =
        previousSnapshot.telemetry.altitude.pressureState;
    const float previousPressureHpa =
        previousState == core::PressureFieldState::Invalid
            ? NAN
            : previousSnapshot.telemetry.altitude.seaLevelPressureHpa;
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t connectionStartedMs = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - connectionStartedMs < 15000u) {
        delay(500);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Logger::error("Weather", "Failed to connect to Wi-Fi");
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    bool success = false;
    if (numCachedStations_ == 0 && !fetchNearestStations(location)) {
        Logger::error("Weather", "Failed to find nearest stations");
        goto cleanup;
    }

    {
        String mapTime;
        int64_t observationUtcMs = 0;
        if (!fetchLatestTime(mapTime, observationUtcMs)) goto cleanup;
        const core::TimeSnapshot clock = hal::Clock::snapshot();
        uint32_t observationAgeMs = 0;
        if (!clock.utcValid || !utils::amedas_policy::observationAgeMs(
                clock.utcEpochUs / 1000LL, observationUtcMs,
                observationAgeMs)) {
            goto cleanup;
        }

        const String url = "https://www.jma.go.jp/bosai/amedas/data/map/" +
            mapTime + ".json";
        WiFiClientSecure client;
        client.setCACert(config::JMA_GLOBALSIGN_ROOT_R46);
        HTTPClient http;
        if (!http.begin(client, url)) goto cleanup;
        const int httpCode = http.GET();
        if (httpCode != HTTP_CODE_OK) {
            Logger::error("Weather", "HTTP GET map json failed: %d", httpCode);
            http.end();
            goto cleanup;
        }

        utils::amedas_policy::StationSample samples[5];
        AmedasStatus nextStatus {};
        nextStatus.location = location;
        nextStatus.observationUtcMs = observationUtcMs;
        nextStatus.observationAgeMs = observationAgeMs;
        nextStatus.cachedStations = static_cast<uint8_t>(numCachedStations_);
        for (int i = 0; i < numCachedStations_; ++i) {
            std::strncpy(nextStatus.stations[i].id,
                nearestStations_[i].id, 5);
            nextStatus.stations[i].id[5] = '\0';
            nextStatus.stations[i].distanceKm =
                nearestStations_[i].distanceKm;
            samples[i].distanceKm = nearestStations_[i].distanceKm;
        }

        WiFiClient* stream = http.getStreamPtr();
        int braceLevel = 0;
        String objectText;
        String currentStation;
        uint8_t buffer[128];
        int remaining = http.getSize();
        uint32_t lastDataMs = millis();
        bool insideRootKey = false;
        int foundCount = 0;

        while ((http.connected() || stream->available()) &&
               millis() - lastDataMs < 15000u) {
            const size_t available = stream->available();
            if (available == 0) {
                delay(1);
                continue;
            }
            lastDataMs = millis();
            const int read = stream->readBytes(buffer,
                available > sizeof(buffer) ? sizeof(buffer) : available);
            for (int i = 0; i < read; ++i) {
                const char ch = static_cast<char>(buffer[i]);
                if (braceLevel == 1 && ch == '"' && !insideRootKey) {
                    insideRootKey = true;
                    currentStation = "";
                } else if (braceLevel == 1 && ch == '"' && insideRootKey) {
                    insideRootKey = false;
                } else if (insideRootKey) {
                    currentStation += ch;
                }
                if (ch == '{') ++braceLevel;
                if (ch == '}') --braceLevel;
                if (braceLevel >= 2) {
                    bool target = false;
                    for (int k = 0; k < numCachedStations_; ++k) {
                        if (currentStation == nearestStations_[k].id) {
                            target = true;
                            break;
                        }
                    }
                    if (target) objectText += ch;
                } else if (braceLevel == 1 && ch == '}') {
                    int targetIndex = -1;
                    for (int k = 0; k < numCachedStations_; ++k) {
                        if (currentStation == nearestStations_[k].id) {
                            targetIndex = k;
                            break;
                        }
                    }
                    if (targetIndex >= 0 && objectText.length() > 0) {
                        float pressureHpa = 0.0f;
                        uint8_t qualityCode = 0xFFu;
                        if (parseNormalPressure(objectText, pressureHpa,
                                                qualityCode)) {
                            samples[targetIndex].seaLevelPressureHpa = pressureHpa;
                            samples[targetIndex].qualityCode = qualityCode;
                            samples[targetIndex].present = true;
                            nextStatus.stations[targetIndex].seaLevelPressureHpa =
                                pressureHpa;
                            nextStatus.stations[targetIndex].qualityCode =
                                qualityCode;
                            nextStatus.stations[targetIndex].used =
                                utils::amedas_policy::usable(samples[targetIndex]);
                            if (nextStatus.stations[targetIndex].used &&
                                nextStatus.usedStations != UINT8_MAX) {
                                ++nextStatus.usedStations;
                            }
                            ++foundCount;
                        }
                    }
                    objectText = "";
                    currentStation = "";
                    if (foundCount >= numCachedStations_) goto map_done;
                }
            }
            if (remaining > 0) {
                remaining -= read;
                if (remaining <= 0) break;
            }
        }
map_done:
        http.end();

        utils::amedas_policy::IdwResult idw;
        if (!utils::amedas_policy::interpolateIdw(
                samples, static_cast<size_t>(numCachedStations_), idw)) {
            Logger::error("Weather",
                "AMeDAS update rejected: fewer than 3 fresh quality=0 pressure stations");
            for (int k = 0; k < numCachedStations_; ++k) {
                Logger::warn("Weather",
                    "AMeDAS rejected station=%s distance_km=%.2f present=%u pressure_hpa=%.1f quality=%u used=%u",
                    nextStatus.stations[k].id,
                    nextStatus.stations[k].distanceKm,
                    samples[k].present ? 1u : 0u,
                    nextStatus.stations[k].seaLevelPressureHpa,
                    nextStatus.stations[k].qualityCode,
                    nextStatus.stations[k].used ? 1u : 0u);
            }
            persistPressureFieldAttempt(
                nextStatus, previousPressureHpa, previousState,
                "REJECTED_MIN_STATIONS");
            goto cleanup;
        }

        nextStatus.state = core::PressureFieldState::Valid;
        nextStatus.interpolatedSeaLevelPressureHpa =
            idw.seaLevelPressureHpa;
        nextStatus.minDistanceKm = idw.minDistanceKm;
        nextStatus.maxDistanceKm = idw.maxDistanceKm;
        nextStatus.usedStations = idw.usedStations;
        nextStatus.lastFetchSucceeded = true;
        persistPressureFieldAttempt(
            nextStatus, previousPressureHpa,
            core::PressureFieldState::Valid, "ACCEPTED");

        portENTER_CRITICAL(&statusMux_);
        status_ = nextStatus;
        lastGoodObservationCapturedMs_ = nowMs - observationAgeMs;
        hasLastGoodField_ = true;
        portEXIT_CRITICAL(&statusMux_);
        sensorManager_.setSeaLevelPressure(
            idw.seaLevelPressureHpa, core::PressureFieldState::Valid,
            core::PressureReferenceSource::Amedas, observationAgeMs,
            idw.usedStations);

        Logger::info("Weather",
            "AMeDAS IDW accepted: P0=%.1f hPa observation_age_ms=%lu used=%u/%d distance=%.1f..%.1f km location=%s",
            idw.seaLevelPressureHpa,
            static_cast<unsigned long>(observationAgeMs),
            idw.usedStations, numCachedStations_,
            idw.minDistanceKm, idw.maxDistanceKm,
            core::locationSourceName(location.source));
        for (int k = 0; k < numCachedStations_; ++k) {
            Logger::info("Weather",
                "AMeDAS station=%s distance_km=%.2f pressure_hpa=%.1f quality=%u used=%u",
                nextStatus.stations[k].id,
                nextStatus.stations[k].distanceKm,
                nextStatus.stations[k].seaLevelPressureHpa,
                nextStatus.stations[k].qualityCode,
                nextStatus.stations[k].used ? 1u : 0u);
        }
        success = true;
    }

cleanup:
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    return success;
}

} // namespace services
