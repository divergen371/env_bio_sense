#include "drivers/sensors/sgp41_sensor.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include <Wire.h>

namespace drivers {
namespace sensors {

Sgp41Sensor::Sgp41Sensor() {}

bool Sgp41Sensor::begin() {
    services::Logger::info("SGP41", "Initializing SGP41...");
    state_ = core::DeviceState::Initializing;
    
    sgp41_.begin(Wire);

    uint16_t error;
    char errorMessage[256];

    // Execute self test
    uint16_t testResult;
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            error = sgp41_.executeConditioning(compensationRh_, compensationT_, srawVoc_);
        } else {
            services::Logger::error("SGP41", "Failed to acquire lock for init");
            return false;
        }
    }
    
    // Test the serial number just to see if it responds
    uint16_t serialNumber[3];
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            error = sgp41_.getSerialNumber(serialNumber);
        } else {
            services::Logger::error("SGP41", "Failed to acquire lock for getSerialNumber");
            return false;
        }
    }

    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SGP41", "Failed to get serial: %s", errorMessage);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    services::Logger::info("SGP41", "SGP41 initialized. Serial: 0x%04X%04X%04X", serialNumber[0], serialNumber[1], serialNumber[2]);

    // Initialize Gas Index Algorithms
    // Default tuning parameters are usually fine, so we don't set them explicitly
    // vocAlgorithm_ / noxAlgorithm_ constructor takes care of initialization if needed

    startMs_ = millis();
    state_ = core::DeviceState::Ready; // Initially it's in conditioning phase but running
    lastError_ = core::ErrorCode::None;
    successCount_ = 0;
    readErrorCount_ = 0;
    consecutiveErrors_ = 0;
    return true;
}

void Sgp41Sensor::setCompensation(float temperatureC, float humidityRh) {
    if (std::isnan(temperatureC) || std::isnan(humidityRh)) {
        compensationRh_ = 0x8000;
        compensationT_ = 0x6666;
        return;
    }

    // Clamp values just in case
    if (humidityRh < 0.0f) humidityRh = 0.0f;
    if (humidityRh > 100.0f) humidityRh = 100.0f;
    if (temperatureC < -45.0f) temperatureC = -45.0f;
    if (temperatureC > 130.0f) temperatureC = 130.0f;

    compensationRh_ = static_cast<uint16_t>(humidityRh * 65535.0f / 100.0f);
    compensationT_ = static_cast<uint16_t>((temperatureC + 45.0f) * 65535.0f / 175.0f);
}

void Sgp41Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }

    uint16_t error = 0;

    // SGP41 needs about 10 seconds of conditioning (measureRawSignals during first 10 seconds will output default)
    // SGP41 measurement period should be 1 second for Gas Index Algorithm to work optimally.
    // SensorManager calls this every 1000ms.
    
    if (nowMs - startMs_ < 10000) {
        // Still in conditioning phase (optional logic, measureRawSignals returns raw signal)
        {
            hal::I2cLockGuard lock(100);
            if (!lock.acquired()) return;
            error = sgp41_.executeConditioning(compensationRh_, compensationT_, srawVoc_);
        }
        
        if (error) {
            readErrorCount_++;
        }
        // Conditioning doesn't give valid NOx or accurate VOC, just skip processing
        hasValidData_ = false;
        return;
    }

    {
        hal::I2cLockGuard lock(100);
        if (!lock.acquired()) {
            services::Logger::warn("SGP41", "ts=%u lock=TIMEOUT read=SKIPPED", nowMs);
            return;
        }
        error = sgp41_.measureRawSignals(compensationRh_, compensationT_, srawVoc_, srawNox_);
    }
    
    if (error) {
        readErrorCount_++;
        consecutiveErrors_++;
        hasValidData_ = false;
        lastError_ = core::ErrorCode::ReadFailed;
        services::Logger::warn("SGP41", "ts=%u read=NG err=%u consec=%u", nowMs, error, consecutiveErrors_);
        
        if (consecutiveErrors_ > 10) {
            state_ = core::DeviceState::Error;
        }
        return;
    }

    // Process raw signals with Gas Index Algorithm
    vocIndex_ = vocAlgorithm_.process(srawVoc_);
    noxIndex_ = noxAlgorithm_.process(srawNox_);

    hasValidData_ = true;
    lastSuccessMs_ = nowMs;
    lastError_ = core::ErrorCode::None;
    consecutiveErrors_ = 0;
    successCount_++;
    
    // Only log occasionally to avoid spam, or log concisely
    if (successCount_ % 10 == 0) {
        services::Logger::info("SGP41", "ts=%u VOCIdx=%d NOxIdx=%d srawVoc=%u srawNox=%u",
            nowMs, vocIndex_, noxIndex_, srawVoc_, srawNox_);
    }
}

bool Sgp41Sensor::readEnvironment(core::EnvironmentData& out) const {
    if (hasValidData_) {
        out.vocIndex = vocIndex_;
        out.noxIndex = noxIndex_;
        out.sgp41Valid = true;
        // Don't override out.timestampMs entirely as other sensors might have updated it,
        // but we assume SensorManager handles overall validity.
        return true;
    }
    out.sgp41Valid = false;
    return false;
}

} // namespace sensors
} // namespace drivers
