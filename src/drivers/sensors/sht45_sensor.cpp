#include "drivers/sensors/sht45_sensor.h"
#include "services/logger.h"
#include <Wire.h>
#include "hal/clock.h"
#include "hal/i2c_bus.h"
#include "utils/sht4x_protocol.h"

namespace drivers {
namespace sensors {

Sht45Sensor::Sht45Sensor() {}

bool Sht45Sensor::begin() {
    services::Logger::info("SHT45", "Initializing SHT45...");
    state_ = core::DeviceState::Initializing;
    
    uint32_t serialNumber;
    uint16_t error = 0;
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Sht45,
                               hal::I2cOperation::Init, 100);
        if (!lock.acquired()) {
            state_ = core::DeviceState::RetryWait;
            lastError_ = core::ErrorCode::Timeout;
            retryAtMs_ = millis() + RETRY_DELAY_MS;
            hasValidData_ = false;
            return false;
        }
        sht4x_.begin(Wire, 0x44);
        error = sht4x_.serialNumber(serialNumber);
    }
    if (error) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Sht45,
                                            hal::I2cOperation::Init);
        services::Logger::error("SHT45", "Failed to read serial number");
        state_ = core::DeviceState::RetryWait;
        lastError_ = core::ErrorCode::InitFailed;
        retryAtMs_ = millis() + RETRY_DELAY_MS;
        hasValidData_ = false;
        return false;
    }

    services::Logger::info("SHT45", "SHT45 initialized. Serial: %lu", serialNumber);
    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    consecutiveErrors_ = 0;
    return true;
}

uint8_t Sht45Sensor::heaterCommand(HeaterPower power,
                                   HeaterDuration duration) {
    if (power == HeaterPower::Highest) {
        return duration == HeaterDuration::Long ? 0x39 : 0x32;
    }
    if (power == HeaterPower::Medium) {
        return duration == HeaterDuration::Long ? 0x2F : 0x24;
    }
    return duration == HeaterDuration::Long ? 0x1E : 0x15;
}

bool Sht45Sensor::deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

bool Sht45Sensor::triggerHeater(HeaterPower power, HeaterDuration duration) {
    if (state_ == core::DeviceState::Error ||
        state_ == core::DeviceState::Offline ||
        state_ == core::DeviceState::RetryWait) {
        return false;
    }

    uint8_t expected = 0;
    return heaterRequest_.compare_exchange_strong(
        expected, heaterCommand(power, duration),
        std::memory_order_release, std::memory_order_relaxed);
}

void Sht45Sensor::markFailure(core::ErrorCode error, uint32_t nowMs,
                              const char* operation) {
    hasValidData_ = false;
    lastError_ = error;
    ++readErrorCount_;
    ++consecutiveErrors_;
    state_ = consecutiveErrors_ >= 3
        ? core::DeviceState::RetryWait : core::DeviceState::Warning;
    if (state_ == core::DeviceState::RetryWait) {
        retryAtMs_ = nowMs + RETRY_DELAY_MS;
    }
    services::Logger::warn("SHT45", "ts=%u op=%s failed consec=%u",
                           nowMs, operation, consecutiveErrors_);
}

bool Sht45Sensor::sendHeaterCommand(uint8_t command) {
    hal::I2cLockGuard lock(hal::I2cDevice::Sht45,
                           hal::I2cOperation::Maintenance, 100);
    if (!lock.acquired()) return false;
    Wire.beginTransmission(0x44);
    Wire.write(command);
    if (Wire.endTransmission() != 0) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Sht45,
                                            hal::I2cOperation::Maintenance);
        return false;
    }
    return true;
}

bool Sht45Sensor::readHeaterResult(float& temperatureC, float& humidityRh) {
    hal::I2cLockGuard lock(hal::I2cDevice::Sht45,
                           hal::I2cOperation::Maintenance, 100);
    if (!lock.acquired()) return false;

    uint8_t frame[6] {};
    const size_t received = Wire.requestFrom(static_cast<uint8_t>(0x44),
                                             static_cast<uint8_t>(6));
    if (received != sizeof(frame) || Wire.available() < sizeof(frame)) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Sht45,
                                            hal::I2cOperation::Maintenance);
        while (Wire.available() > 0) Wire.read();
        return false;
    }
    for (uint8_t& byte : frame) byte = static_cast<uint8_t>(Wire.read());
    if (!utils::sht4x::decodeMeasurement(frame, temperatureC, humidityRh)) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Sht45,
                                            hal::I2cOperation::Maintenance);
        return false;
    }
    return true;
}

void Sht45Sensor::processHeater(uint32_t nowMs) {
    if (heaterPhase_ == HeaterPhase::Idle) {
        const uint8_t command = heaterRequest_.load(std::memory_order_acquire);
        if (command == 0 || command == HEATER_ACTIVE) return;
        if (!sendHeaterCommand(command)) {
            heaterRequest_.store(0, std::memory_order_release);
            markFailure(core::ErrorCode::BusError, nowMs, "heater-command");
            return;
        }
        hasValidData_ = false;
        heaterRequest_.store(HEATER_ACTIVE, std::memory_order_release);
        heaterPhase_ = HeaterPhase::Waiting;
        const bool longDuration = command == 0x39 || command == 0x2F ||
                                  command == 0x1E;
        heaterDeadlineMs_ = nowMs + (longDuration ? 1100u : 110u);
        services::Logger::info("SHT45", "Heater command accepted (0x%02X)",
                               command);
        return;
    }

    if (!deadlineReached(nowMs, heaterDeadlineMs_)) return;
    if (heaterPhase_ == HeaterPhase::Waiting) {
        float heaterTemperature = 0.0f;
        float heaterHumidity = 0.0f;
        if (!readHeaterResult(heaterTemperature, heaterHumidity)) {
            heaterPhase_ = HeaterPhase::Idle;
            heaterRequest_.store(0, std::memory_order_release);
            markFailure(core::ErrorCode::ReadFailed, nowMs, "heater-result");
            return;
        }
        services::Logger::info(
            "SHT45", "Heater completed. Heated sample: %.2fC, %.2f%%RH",
            heaterTemperature, heaterHumidity);
        heaterPhase_ = HeaterPhase::Cooldown;
        heaterDeadlineMs_ = nowMs + HEATER_COOLDOWN_MS;
        return;
    }

    heaterPhase_ = HeaterPhase::Idle;
    heaterRequest_.store(0, std::memory_order_release);
    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    consecutiveErrors_ = 0;
}

void Sht45Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Offline ||
        state_ == core::DeviceState::Error ||
        state_ == core::DeviceState::RetryWait) {
        if (!deadlineReached(nowMs, retryAtMs_)) return;
        begin();
        return;
    }

    processHeater(nowMs);
    if (heaterRequest_.load(std::memory_order_acquire) != 0) return;

    float temperature = 0;
    float humidity = 0;
    uint16_t error = 0;
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Sht45,
                               hal::I2cOperation::Measure, 100);
        if (!lock.acquired()) {
            markFailure(core::ErrorCode::Timeout, nowMs, "measure-lock");
            return;
        }
        error = sht4x_.measureHighPrecision(temperature, humidity);
    }
    if (error) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Sht45,
                                            hal::I2cOperation::Measure);
        markFailure(core::ErrorCode::ReadFailed, nowMs, "measure");
    } else {
        currentTemperature_ = temperature;
        currentHumidity_ = humidity;
        hasValidData_ = true;
        lastSuccessMs_ = nowMs;
        lastError_ = core::ErrorCode::None;
        state_ = core::DeviceState::Ready;
        consecutiveErrors_ = 0;
        ++successCount_;
    }
}

bool Sht45Sensor::readEnvironment(core::EnvironmentData& out) const {
    if (!hasValidData_) return false;
    const uint32_t ageMs = millis() - lastSuccessMs_;
    if (ageMs > DATA_MAX_AGE_MS) return false;

    out.temperatureC = currentTemperature_;
    out.humidityRh = currentHumidity_;
    
    out.timestampMs = lastSuccessMs_;
    out.valid = true;
    return true;
}

} // namespace sensors
} // namespace drivers
