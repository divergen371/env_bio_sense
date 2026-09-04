#include "hal/i2c_bus.h"
#include "hal/pins.h"
#include "services/logger.h"
#include <Arduino.h>
#include <Wire.h>

namespace hal {

SemaphoreHandle_t I2cBus::mutex_ = nullptr;
portMUX_TYPE I2cBus::diagnosticsMux_ = portMUX_INITIALIZER_UNLOCKED;
I2cDiagnosticCounters I2cBus::diagnostics_[
    static_cast<uint8_t>(I2cDevice::Count)][
    static_cast<uint8_t>(I2cOperation::Count)] {};

namespace {

void saturatingIncrement(uint32_t& value) {
    if (value != UINT32_MAX) ++value;
}

bool validDiagnosticKey(I2cDevice device, I2cOperation operation) {
    return static_cast<uint8_t>(device) < static_cast<uint8_t>(I2cDevice::Count) &&
           static_cast<uint8_t>(operation) < static_cast<uint8_t>(I2cOperation::Count);
}

} // namespace

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

void I2cBus::noteLockTimeout(I2cDevice device, I2cOperation operation) {
    if (!validDiagnosticKey(device, operation)) return;
    portENTER_CRITICAL(&diagnosticsMux_);
    saturatingIncrement(diagnostics_[static_cast<uint8_t>(device)]
        [static_cast<uint8_t>(operation)].lockTimeouts);
    portEXIT_CRITICAL(&diagnosticsMux_);
}

void I2cBus::noteCommunicationError(I2cDevice device, I2cOperation operation) {
    if (!validDiagnosticKey(device, operation)) return;
    portENTER_CRITICAL(&diagnosticsMux_);
    saturatingIncrement(diagnostics_[static_cast<uint8_t>(device)]
        [static_cast<uint8_t>(operation)].communicationErrors);
    portEXIT_CRITICAL(&diagnosticsMux_);
}

I2cDiagnosticCounters I2cBus::diagnostics(I2cDevice device,
                                           I2cOperation operation) {
    I2cDiagnosticCounters result {};
    if (!validDiagnosticKey(device, operation)) return result;
    portENTER_CRITICAL(&diagnosticsMux_);
    result = diagnostics_[static_cast<uint8_t>(device)]
        [static_cast<uint8_t>(operation)];
    portEXIT_CRITICAL(&diagnosticsMux_);
    return result;
}

I2cDiagnosticCounters I2cBus::diagnosticTotals() {
    I2cDiagnosticCounters result {};
    portENTER_CRITICAL(&diagnosticsMux_);
    for (uint8_t device = 0;
         device < static_cast<uint8_t>(I2cDevice::Count); ++device) {
        for (uint8_t operation = 0;
             operation < static_cast<uint8_t>(I2cOperation::Count); ++operation) {
            const I2cDiagnosticCounters& item = diagnostics_[device][operation];
            const uint32_t lockRoom = UINT32_MAX - result.lockTimeouts;
            result.lockTimeouts += item.lockTimeouts > lockRoom
                ? lockRoom : item.lockTimeouts;
            const uint32_t errorRoom = UINT32_MAX - result.communicationErrors;
            result.communicationErrors += item.communicationErrors > errorRoom
                ? errorRoom : item.communicationErrors;
        }
    }
    portEXIT_CRITICAL(&diagnosticsMux_);
    return result;
}

I2cLockGuard::I2cLockGuard(uint32_t timeoutMs) {
    acquired_ = I2cBus::lock(timeoutMs);
}

I2cLockGuard::I2cLockGuard(I2cDevice device, I2cOperation operation,
                           uint32_t timeoutMs) {
    acquired_ = I2cBus::lock(timeoutMs);
    if (!acquired_) I2cBus::noteLockTimeout(device, operation);
}

I2cLockGuard::~I2cLockGuard() {
    if (acquired_) {
        I2cBus::unlock();
    }
}

} // namespace hal
