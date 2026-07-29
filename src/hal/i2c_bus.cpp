#include "hal/i2c_bus.h"
#include "hal/pins.h"
#include "services/logger.h"
#include <Arduino.h>
#include <Wire.h>

namespace hal {

bool I2cBus::begin() {
    Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
    Wire.setClock(100000); // 初期は100kHz
    return true;
}

void I2cBus::scan() {
    services::Logger::info("I2C", "I2C scan started.");

    uint8_t deviceCount = 0;

    for (uint8_t address = 1; address < 127; ++address) {
        Wire.beginTransmission(address);
        const uint8_t error = Wire.endTransmission();

        if (error == 0) {
            services::Logger::info("I2C", "Found device at 0x%02X", address);
            ++deviceCount;
        } else if (error == 4) {
            services::Logger::warn("I2C", "Unknown error at 0x%02X", address);
        }
    }

    if (deviceCount == 0) {
        services::Logger::warn("I2C", "No I2C devices found.");
    } else {
        services::Logger::info("I2C", "Scan complete: %u device(s) found.", deviceCount);
    }
}

} // namespace hal
