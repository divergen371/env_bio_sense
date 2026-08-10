#pragma once

#include <Arduino.h>
#include <cstdint>

namespace hal {
namespace pins {

constexpr uint8_t I2C_SDA = D4;       // GPIO6
constexpr uint8_t I2C_SCL = D5;       // GPIO7
constexpr uint8_t MAX30102_INT = D2;  // GPIO2
constexpr uint8_t BMP5_INT = D3;    // GPIO3
constexpr uint8_t SD_CS = D7;         // SD Card Chip Select
constexpr uint8_t USER_LED = 21;      // Built-in User LED

} // namespace pins
} // namespace hal
