#pragma once

#include <cstddef>
#include <cstdint>

namespace utils {
namespace sht4x {

inline uint8_t crc8(const uint8_t* data, size_t length) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80u) != 0
                ? static_cast<uint8_t>((crc << 1u) ^ 0x31u)
                : static_cast<uint8_t>(crc << 1u);
        }
    }
    return crc;
}

inline bool decodeMeasurement(const uint8_t (&frame)[6],
                              float& temperatureC,
                              float& humidityRh) {
    if (crc8(frame, 2) != frame[2] || crc8(frame + 3, 2) != frame[5]) {
        return false;
    }

    const uint16_t temperatureTicks =
        static_cast<uint16_t>((static_cast<uint16_t>(frame[0]) << 8u) |
                              frame[1]);
    const uint16_t humidityTicks =
        static_cast<uint16_t>((static_cast<uint16_t>(frame[3]) << 8u) |
                              frame[4]);
    temperatureC = (static_cast<float>(temperatureTicks) * 175.0f /
                    65535.0f) - 45.0f;
    humidityRh = (static_cast<float>(humidityTicks) * 125.0f /
                  65535.0f) - 6.0f;
    if (humidityRh < 0.0f) humidityRh = 0.0f;
    if (humidityRh > 100.0f) humidityRh = 100.0f;
    return true;
}

} // namespace sht4x
} // namespace utils
