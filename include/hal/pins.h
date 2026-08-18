#pragma once

#include <Arduino.h>
#include <cstdint>

namespace hal {
namespace pins {

constexpr uint8_t I2C_SDA = D4;       // GPIO6
constexpr uint8_t I2C_SCL = D5;       // GPIO7
constexpr uint8_t MAX30102_INT = D2;
constexpr uint8_t BMP5_INT = D3;    // GPIO3
constexpr uint8_t SD_CS = 2;          // SD Card Chip Select (GPIO2 / D1)
constexpr uint8_t GNSS_PPS = D0;      // LC76G PPS (D0)
constexpr uint8_t GNSS_TX = D6;       // LC76G UART TX (GPIO43 - XIAO default TX)
constexpr uint8_t GNSS_RX = D7;       // LC76G UART RX (GPIO44 - XIAO default RX, previously SD_CS)
constexpr uint8_t USER_LED = 21;      // Built-in User LED
constexpr uint8_t BOOT_BUTTON = 0;    // BOOT button on XIAO ESP32S3

} // namespace pins
} // namespace hal
