#pragma once

#include "utils/utc_time.h"
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace utils {
namespace amedas_policy {

constexpr uint32_t OBSERVATION_MAX_AGE_MS = 15u * 60u * 1000u;
constexpr uint32_t FUTURE_TOLERANCE_MS = 2u * 60u * 1000u;
constexpr uint32_t LAST_KNOWN_MAX_AGE_MS = 60u * 60u * 1000u;
constexpr uint8_t MIN_VALID_STATIONS = 3u;

struct StationSample {
    float seaLevelPressureHpa {};
    float distanceKm {};
    uint8_t qualityCode {0xFFu};
    bool present {false};

    StationSample() = default;
    StationSample(float pressureHpa, float distance,
                  uint8_t quality, bool isPresent)
        : seaLevelPressureHpa(pressureHpa),
          distanceKm(distance),
          qualityCode(quality),
          present(isPresent) {}
};

struct IdwResult {
    float seaLevelPressureHpa {};
    float minDistanceKm {};
    float maxDistanceKm {};
    uint8_t usedStations {};
};

inline int twoDigits(const char* text) {
    if (text[0] < '0' || text[0] > '9' ||
        text[1] < '0' || text[1] > '9') return -1;
    return (text[0] - '0') * 10 + (text[1] - '0');
}

inline bool parseJstIso8601(const char* text, int64_t& utcEpochMs) {
    if (text == nullptr) return false;
    for (size_t i = 0; i < 25; ++i) {
        if (text[i] == '\0') return false;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != '+' ||
        text[22] != ':' || text[25] != '\0') {
        return false;
    }
    const int y0 = twoDigits(text);
    const int y1 = twoDigits(text + 2);
    const int month = twoDigits(text + 5);
    const int day = twoDigits(text + 8);
    const int hour = twoDigits(text + 11);
    const int minute = twoDigits(text + 14);
    const int second = twoDigits(text + 17);
    const int offsetHour = twoDigits(text + 20);
    const int offsetMinute = twoDigits(text + 23);
    if (y0 < 0 || y1 < 0 || month < 0 || day < 0 || hour < 0 ||
        minute < 0 || second < 0 || offsetHour < 0 || offsetMinute < 0 ||
        offsetHour > 23 || offsetMinute > 59) {
        return false;
    }
    int64_t localEpochMs = 0;
    if (!utils::utcEpochMsFromCalendar(y0 * 100 + y1,
            static_cast<uint8_t>(month), static_cast<uint8_t>(day),
            static_cast<uint8_t>(hour), static_cast<uint8_t>(minute),
            static_cast<uint8_t>(second), 0, localEpochMs)) {
        return false;
    }
    utcEpochMs = localEpochMs -
        (static_cast<int64_t>(offsetHour) * 60LL + offsetMinute) * 60000LL;
    return true;
}

inline bool observationAgeMs(int64_t nowUtcMs, int64_t observationUtcMs,
                             uint32_t& ageMs) {
    const int64_t deltaMs = nowUtcMs - observationUtcMs;
    if (deltaMs < -static_cast<int64_t>(FUTURE_TOLERANCE_MS)) return false;
    if (deltaMs <= 0) {
        ageMs = 0;
        return true;
    }
    if (deltaMs > static_cast<int64_t>(OBSERVATION_MAX_AGE_MS)) return false;
    ageMs = static_cast<uint32_t>(deltaMs);
    return true;
}

inline bool usable(const StationSample& sample) {
    return sample.present && sample.qualityCode == 0u &&
           std::isfinite(sample.seaLevelPressureHpa) &&
           sample.seaLevelPressureHpa > 800.0f &&
           sample.seaLevelPressureHpa < 1100.0f &&
           std::isfinite(sample.distanceKm) && sample.distanceKm >= 0.0f;
}

inline bool interpolateIdw(const StationSample* samples, size_t count,
                           IdwResult& result) {
    if (samples == nullptr) return false;
    double weightedPressure = 0.0;
    double weightSum = 0.0;
    float minPressure = INFINITY;
    float maxPressure = -INFINITY;
    float minDistance = INFINITY;
    float maxDistance = 0.0f;
    bool exact = false;
    float exactPressure = 0.0f;
    uint8_t used = 0;

    for (size_t i = 0; i < count; ++i) {
        if (!usable(samples[i])) continue;
        if (used != UINT8_MAX) ++used;
        const float pressure = samples[i].seaLevelPressureHpa;
        const float distance = samples[i].distanceKm;
        if (pressure < minPressure) minPressure = pressure;
        if (pressure > maxPressure) maxPressure = pressure;
        if (distance < minDistance) minDistance = distance;
        if (distance > maxDistance) maxDistance = distance;
        if (distance < 0.001f) {
            exact = true;
            exactPressure = pressure;
        } else {
            const double weight = 1.0 /
                (static_cast<double>(distance) * distance);
            weightSum += weight;
            weightedPressure += weight * pressure;
        }
    }

    if (used < MIN_VALID_STATIONS) return false;
    float pressure = exact ? exactPressure
        : static_cast<float>(weightedPressure / weightSum);
    if (!std::isfinite(pressure) || (!exact && weightSum <= 0.0)) return false;
    if (pressure < minPressure) pressure = minPressure;
    if (pressure > maxPressure) pressure = maxPressure;
    result.seaLevelPressureHpa = pressure;
    result.minDistanceKm = minDistance;
    result.maxDistanceKm = maxDistance;
    result.usedStations = used;
    return true;
}

} // namespace amedas_policy
} // namespace utils
