#include "hal/i2c_bus.h"
#include "hal/pins.h"
#include "services/logger.h"
#include <Arduino.h>
#include <Wire.h>

namespace hal {

SemaphoreHandle_t I2cBus::mutex_ = nullptr;

bool I2cBus::begin() {
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateRecursiveMutex();
    }
    Wire.begin(pins::I2C_SDA, pins::I2C_SCL, 400000); // 400kHz (Fast Mode) に設定
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

bool I2cBus::lock(uint32_t timeoutMs) {
    if (mutex_ == nullptr) return true; // not initialized yet
    return xSemaphoreTakeRecursive(mutex_, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void I2cBus::unlock() {
    if (mutex_ != nullptr) {
        xSemaphoreGiveRecursive(mutex_);
    }
}

I2cLockGuard::I2cLockGuard(uint32_t timeoutMs) {
    acquired_ = I2cBus::lock(timeoutMs);
}

I2cLockGuard::~I2cLockGuard() {
    if (acquired_) {
        I2cBus::unlock();
    }
}

} // namespace hal
