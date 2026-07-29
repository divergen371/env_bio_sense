#pragma once

#include <Arduino.h>
#include <cstdint>

namespace hal {
namespace pins {

constexpr uint8_t I2C_SDA = D4;       // GPIO6
constexpr uint8_t I2C_SCL = D5;       // GPIO7
constexpr uint8_t MAX30102_INT = D2;  // GPIO2
constexpr uint8_t BMP585_INT = D3;    // GPIO3

} // namespace pins
} // namespace hal
