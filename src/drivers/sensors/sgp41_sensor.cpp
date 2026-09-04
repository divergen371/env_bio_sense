#include "drivers/sensors/sgp41_sensor.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include <Wire.h>

namespace drivers {
namespace sensors {

Sgp41Sensor::Sgp41Sensor() {}

bool Sgp41Sensor::deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

void Sgp41Sensor::markFailure(core::ErrorCode error, uint32_t nowMs,
                              const char* operation,
                              bool communicationError) {
    if (communicationError) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Sgp41,
                                            hal::I2cOperation::Measure);
    }
    hasValidData_ = false;
    lastError_ = error;
    ++readErrorCount_;
    ++consecutiveErrors_;
    state_ = consecutiveErrors_ >= MAX_CONSECUTIVE_ERRORS
        ? core::DeviceState::RetryWait : core::DeviceState::Warning;
    if (state_ == core::DeviceState::RetryWait) {
        retryAtMs_ = nowMs + RETRY_DELAY_MS;
    }
    services::Logger::warn("SGP41", "ts=%u op=%s failed consec=%u",
                           nowMs, operation, consecutiveErrors_);
}

bool Sgp41Sensor::begin() {
    services::Logger::info("SGP41", "Initializing SGP41...");
    state_ = core::DeviceState::Initializing;
    
    uint16_t error = 0;
    char errorMessage[256];

    // SGP41 requires an initial conditioning command before normal sampling.
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Sgp41,
                               hal::I2cOperation::Init, 100);
        if (lock.acquired()) {
            sgp41_.begin(Wire);
            error = sgp41_.executeConditioning(compensationRh_, compensationT_, srawVoc_);
        } else {
            services::Logger::error("SGP41", "Failed to acquire lock for init");
            state_ = core::DeviceState::RetryWait;
            lastError_ = core::ErrorCode::Timeout;
            retryAtMs_ = millis() + RETRY_DELAY_MS;
            hasValidData_ = false;
            return false;
        }
    }
    if (error) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Sgp41,
                                            hal::I2cOperation::Init);
        errorToString(error, errorMessage, sizeof(errorMessage));
        services::Logger::error("SGP41", "Conditioning failed: %s", errorMessage);
        state_ = core::DeviceState::RetryWait;
        lastError_ = core::ErrorCode::InitFailed;
        retryAtMs_ = millis() + RETRY_DELAY_MS;
        hasValidData_ = false;
        return false;
    }
    
    // Test the serial number just to see if it responds
    uint16_t serialNumber[3];
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Sgp41,
                               hal::I2cOperation::Init, 100);
        if (lock.acquired()) {
            error = sgp41_.getSerialNumber(serialNumber);
        } else {
            services::Logger::error("SGP41", "Failed to acquire lock for getSerialNumber");
            state_ = core::DeviceState::RetryWait;
            lastError_ = core::ErrorCode::Timeout;
            retryAtMs_ = millis() + RETRY_DELAY_MS;
            hasValidData_ = false;
            return false;
        }
    }

    if (error) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Sgp41,
                                            hal::I2cOperation::Init);
        errorToString(error, errorMessage, sizeof(errorMessage));
        services::Logger::error("SGP41", "Failed to get serial: %s", errorMessage);
        state_ = core::DeviceState::RetryWait;
        lastError_ = core::ErrorCode::InitFailed;
        retryAtMs_ = millis() + RETRY_DELAY_MS;
        hasValidData_ = false;
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
    if (state_ == core::DeviceState::Error ||
        state_ == core::DeviceState::Offline ||
        state_ == core::DeviceState::RetryWait) {
        if (deadlineReached(nowMs, retryAtMs_)) begin();
        return;
    }

    uint16_t error = 0;

    // SGP41 needs about 10 seconds of conditioning (measureRawSignals during first 10 seconds will output default)
    // SGP41 measurement period should be 1 second for Gas Index Algorithm to work optimally.
    // SensorManager calls this every 1000ms.
    
    if (nowMs - startMs_ < 10000) {
        // Still in conditioning phase (optional logic, measureRawSignals returns raw signal)
        {
            hal::I2cLockGuard lock(hal::I2cDevice::Sgp41,
                                   hal::I2cOperation::Measure, 100);
            if (!lock.acquired()) {
                markFailure(core::ErrorCode::Timeout, nowMs,
                            "conditioning-lock", false);
                return;
            }
            error = sgp41_.executeConditioning(compensationRh_, compensationT_, srawVoc_);
        }
        
        if (error) {
            markFailure(core::ErrorCode::ReadFailed, nowMs,
                        "conditioning", true);
            return;
        }
        // Conditioning doesn't give valid NOx or accurate VOC, just skip processing
        hasValidData_ = false;
        state_ = core::DeviceState::Ready;
        lastError_ = core::ErrorCode::None;
        consecutiveErrors_ = 0;
        return;
    }

    {
        hal::I2cLockGuard lock(hal::I2cDevice::Sgp41,
                               hal::I2cOperation::Measure, 100);
        if (!lock.acquired()) {
            markFailure(core::ErrorCode::Timeout, nowMs, "measure-lock", false);
            return;
        }
        error = sgp41_.measureRawSignals(compensationRh_, compensationT_, srawVoc_, srawNox_);
    }
    
    if (error) {
        markFailure(core::ErrorCode::ReadFailed, nowMs, "measure", true);
        return;
    }

    // Process raw signals with Gas Index Algorithm
    vocIndex_ = vocAlgorithm_.process(srawVoc_);
    noxIndex_ = noxAlgorithm_.process(srawNox_);

    hasValidData_ = true;
    lastSuccessMs_ = nowMs;
    lastError_ = core::ErrorCode::None;
    state_ = core::DeviceState::Ready;
    consecutiveErrors_ = 0;
    successCount_++;
    
    // Only log occasionally to avoid spam, or log concisely
    if (successCount_ % 10 == 0) {
        services::Logger::info("SGP41", "ts=%u VOCIdx=%d NOxIdx=%d srawVoc=%u srawNox=%u",
            nowMs, vocIndex_, noxIndex_, srawVoc_, srawNox_);
    }
}

bool Sgp41Sensor::readEnvironment(core::EnvironmentData& out) const {
    if (hasValidData_ && millis() - lastSuccessMs_ <= DATA_MAX_AGE_MS) {
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

void Sgp41Sensor::getAlgorithmStates(float& voc0, float& voc1) const {
    const_cast<VOCGasIndexAlgorithm*>(&vocAlgorithm_)->get_states(voc0, voc1);
}

void Sgp41Sensor::setAlgorithmStates(float voc0, float voc1) {
    vocAlgorithm_.set_states(voc0, voc1);
}

void Sgp41Sensor::getRawTelemetry(uint16_t& srawVoc, uint16_t& srawNox,
                                  uint16_t& compensationRh,
                                  uint16_t& compensationTemperature) const {
    srawVoc = srawVoc_;
    srawNox = srawNox_;
    compensationRh = compensationRh_;
    compensationTemperature = compensationT_;
}

} // namespace sensors
} // namespace drivers
