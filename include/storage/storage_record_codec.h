#pragma once

#include "core/sensor_types.h"
#include <cmath>
#include <cstdint>
#include <limits>

namespace storage {
namespace codec {

constexpr uint8_t SENSOR_RECORD_V6_TAG = 6;

inline int16_t signedFixed(float value, float scale) {
    if (!std::isfinite(value)) return 0;
    const float scaled = value * scale;
    if (scaled <= static_cast<float>(std::numeric_limits<int16_t>::min())) {
        return std::numeric_limits<int16_t>::min();
    }
    if (scaled >= static_cast<float>(std::numeric_limits<int16_t>::max())) {
        return std::numeric_limits<int16_t>::max();
    }
    return static_cast<int16_t>(std::lround(scaled));
}

inline uint16_t unsignedFixed(float value, float scale) {
    if (!std::isfinite(value) || value <= 0.0f) return 0;
    const float scaled = value * scale;
    if (scaled >= static_cast<float>(UINT16_MAX - 1u)) return UINT16_MAX - 1u;
    return static_cast<uint16_t>(std::lround(scaled));
}

inline uint32_t unsignedWhole(float value) {
    if (!std::isfinite(value) || value <= 0.0f) return 0;
    if (value >= static_cast<float>(UINT32_MAX)) return UINT32_MAX;
    return static_cast<uint32_t>(std::lround(value));
}

inline uint16_t ageSeconds(uint32_t ageMs) {
    if (ageMs == UINT32_MAX) return UINT16_MAX;
    const uint32_t seconds = ageMs / 1000u;
    return static_cast<uint16_t>(seconds >= UINT16_MAX
        ? UINT16_MAX - 1u : seconds);
}

inline uint16_t milliseconds16(uint32_t milliseconds) {
    if (milliseconds == UINT32_MAX) return UINT16_MAX;
    return static_cast<uint16_t>(milliseconds >= UINT16_MAX
        ? UINT16_MAX - 1u : milliseconds);
}

inline uint16_t count16(uint32_t count) {
    return static_cast<uint16_t>(count > UINT16_MAX ? UINT16_MAX : count);
}

inline uint8_t packHealth(core::DeviceState state, core::ErrorCode error,
                          uint32_t consecutiveErrors) {
    const uint8_t stateBits = static_cast<uint8_t>(state) & 0x07u;
    const uint8_t errorBits = static_cast<uint8_t>(error) & 0x07u;
    const uint8_t consecutiveBits = static_cast<uint8_t>(
        consecutiveErrors > 3u ? 3u : consecutiveErrors);
    return static_cast<uint8_t>(stateBits | (errorBits << 3u) |
                                (consecutiveBits << 6u));
}

inline core::DeviceState healthState(uint8_t packed) {
    return static_cast<core::DeviceState>(packed & 0x07u);
}

inline core::ErrorCode healthError(uint8_t packed) {
    return static_cast<core::ErrorCode>((packed >> 3u) & 0x07u);
}

inline uint8_t healthConsecutiveErrors(uint8_t packed) {
    return static_cast<uint8_t>((packed >> 6u) & 0x03u);
}

inline uint8_t packSources(core::TimeSource timeSource,
                           core::PressureFieldState pressureState,
                           core::PressureReferenceSource pressureSource) {
    return static_cast<uint8_t>(
        (static_cast<uint8_t>(timeSource) & 0x07u) |
        ((static_cast<uint8_t>(pressureState) & 0x03u) << 3u) |
        ((static_cast<uint8_t>(pressureSource) & 0x07u) << 5u));
}

inline core::TimeSource timeSource(uint8_t packed) {
    return static_cast<core::TimeSource>(packed & 0x07u);
}

inline core::PressureFieldState pressureState(uint8_t packed) {
    return static_cast<core::PressureFieldState>((packed >> 3u) & 0x03u);
}

inline core::PressureReferenceSource pressureSource(uint8_t packed) {
    return static_cast<core::PressureReferenceSource>((packed >> 5u) & 0x07u);
}

} // namespace codec
} // namespace storage
