#pragma once

#include <cmath>

namespace utils {
namespace altitude_math {

constexpr float TEMPERATURE_LAPSE_RATE = 0.0065f;
constexpr float PRESSURE_EXPONENT = 0.190295f;
constexpr float INVERSE_PRESSURE_EXPONENT = 5.2549988f;
constexpr float DISPLAY_DEADBAND_M = 0.5f;

inline bool expectedPressureHpa(float seaLevelPressureHpa,
                                float temperatureC,
                                float altitudeM,
                                float& expectedHpa) {
    if (!std::isfinite(seaLevelPressureHpa) || seaLevelPressureHpa <= 0.0f ||
        !std::isfinite(temperatureC) || !std::isfinite(altitudeM)) {
        return false;
    }
    const float temperatureK = temperatureC + 273.15f;
    const float base = 1.0f - TEMPERATURE_LAPSE_RATE * altitudeM /
        temperatureK;
    if (!std::isfinite(temperatureK) || temperatureK <= 0.0f ||
        !std::isfinite(base) || base <= 0.0f) {
        return false;
    }
    expectedHpa = seaLevelPressureHpa *
        std::pow(base, INVERSE_PRESSURE_EXPONENT);
    return std::isfinite(expectedHpa) && expectedHpa > 0.0f;
}

inline bool absoluteAltitudeM(float rawPressureHpa,
                              float pressureOffsetHpa,
                              float seaLevelPressureHpa,
                              float temperatureC,
                              float& altitudeM) {
    if (!std::isfinite(rawPressureHpa) ||
        !std::isfinite(pressureOffsetHpa) ||
        !std::isfinite(seaLevelPressureHpa) ||
        !std::isfinite(temperatureC)) {
        return false;
    }
    const float correctedPressureHpa = rawPressureHpa - pressureOffsetHpa;
    const float temperatureK = temperatureC + 273.15f;
    const float ratio = correctedPressureHpa / seaLevelPressureHpa;
    if (correctedPressureHpa <= 0.0f || seaLevelPressureHpa <= 0.0f ||
        temperatureK <= 0.0f || !std::isfinite(ratio) || ratio <= 0.0f) {
        return false;
    }
    altitudeM = (temperatureK / TEMPERATURE_LAPSE_RATE) *
        (1.0f - std::pow(ratio, PRESSURE_EXPONENT));
    return std::isfinite(altitudeM);
}

inline float applyDisplayHysteresis(float rawAltitudeM,
                                    float previousDisplayM,
                                    bool hasPrevious) {
    if (!hasPrevious || !std::isfinite(previousDisplayM)) return rawAltitudeM;
    const float differenceM = rawAltitudeM - previousDisplayM;
    if (differenceM > DISPLAY_DEADBAND_M) {
        return rawAltitudeM - DISPLAY_DEADBAND_M;
    }
    if (differenceM < -DISPLAY_DEADBAND_M) {
        return rawAltitudeM + DISPLAY_DEADBAND_M;
    }
    return previousDisplayM;
}

} // namespace altitude_math
} // namespace utils
