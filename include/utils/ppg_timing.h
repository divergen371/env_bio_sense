#pragma once

#include <cstdint>

namespace utils {
namespace ppg_timing {

constexpr uint16_t SENSOR_SAMPLE_RATE_HZ = 400;
constexpr uint8_t SAMPLE_AVERAGE = 4;
constexpr uint16_t EFFECTIVE_SAMPLE_RATE_HZ =
    SENSOR_SAMPLE_RATE_HZ / SAMPLE_AVERAGE;
constexpr uint32_t SAMPLE_INTERVAL_MS =
    1000u / EFFECTIVE_SAMPLE_RATE_HZ;

inline uint32_t droppedFromLibraryBuffer(uint16_t fetched,
                                         uint8_t buffered) {
    return fetched > buffered
        ? static_cast<uint32_t>(fetched - buffered) : 0u;
}

inline uint32_t bufferedSampleTimestampMs(uint32_t drainTimeMs,
                                          uint8_t bufferedCount,
                                          uint8_t sampleOffset) {
    if (bufferedCount == 0 || sampleOffset >= bufferedCount) {
        return drainTimeMs;
    }
    return drainTimeMs - static_cast<uint32_t>(
        bufferedCount - 1u - sampleOffset) * SAMPLE_INTERVAL_MS;
}

} // namespace ppg_timing
} // namespace utils
