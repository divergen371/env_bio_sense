#pragma once

#include <cstdint>

namespace utils {
namespace scd41_policy {

constexpr uint32_t MIN_FRC_MEASUREMENT_MS = 180000u;
constexpr uint32_t MAX_PRESSURE_AGE_MS = 15000u;
constexpr uint32_t PRESSURE_KEEPALIVE_MS = 60000u;
constexpr uint8_t RECOVERY_GOOD_SAMPLES_REQUIRED = 3u;
constexpr uint32_t RECOVERY_STABLE_RESET_MS = 300000u;

inline bool decodeFrcCorrection(uint16_t rawWord, int16_t& correctionPpm) {
    if (rawWord == 0xFFFFu) return false;
    correctionPpm = static_cast<int16_t>(
        static_cast<int32_t>(rawWord) - 0x8000L);
    return true;
}

inline bool frcPreconditions(bool maintenanceInProgress,
                             bool sensorHealthy,
                             uint32_t measurementUptimeMs,
                             bool hasValidCo2,
                             uint16_t referencePpm,
                             bool hasAmbientPressure,
                             uint32_t pressureAgeMs) {
    return !maintenanceInProgress && sensorHealthy &&
           measurementUptimeMs >= MIN_FRC_MEASUREMENT_MS &&
           hasValidCo2 && referencePpm >= 400u && referencePpm <= 5000u &&
           hasAmbientPressure && pressureAgeMs <= MAX_PRESSURE_AGE_MS;
}

inline bool recoveryUsesReinit(uint8_t attemptsSinceStable) {
    return attemptsSinceStable >= 1u;
}

inline uint32_t recoveryBackoffMs(uint8_t attemptsSinceStable) {
    if (attemptsSinceStable <= 1u) return 30000u;
    if (attemptsSinceStable == 2u) return 120000u;
    return 300000u;
}

inline bool recoveryQuarantineComplete(uint8_t consecutiveGoodSamples) {
    return consecutiveGoodSamples >= RECOVERY_GOOD_SAMPLES_REQUIRED;
}

} // namespace scd41_policy
} // namespace utils
