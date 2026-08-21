#pragma once

#include <Arduino.h>
#include <cstdint>

namespace hal {
namespace pins {

    constexpr uint8_t I2C_SDA = D4;       // GPIO6
    constexpr uint8_t I2C_SCL = D5;       // GPIO7
    constexpr uint8_t BMP5_INT = D3;      // GPIO3
constexpr uint8_t SD_CS = 2;          // SD Card Chip Select (GPIO2 / D1)
constexpr uint8_t GNSS_PPS = D0;      // GPIO1, LC76G PPS -> XIAO
constexpr uint8_t GNSS_UART_TX = D6;  // GPIO43, XIAO TX -> LC76G RX
    constexpr uint8_t GNSS_UART_RX = D7;  // GPIO44, XIAO RX <- LC76G TX
    constexpr uint8_t USER_LED = 21;      // Built-in User LED
    constexpr uint8_t ACTION_BUTTON = D2; // Tact switch (Active High via 10k to 3.3V)

} // namespace pins
} // namespace hal
