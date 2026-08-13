#include "drivers/sensors/bmp5_sensor_base.h"
#include "drivers/sensors/bmp5_diagnostic.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include <Wire.h>
#include <Arduino.h>
#include <cmath>

namespace drivers {
namespace sensors {

Bmp5SensorBase::Bmp5SensorBase() {}

// --- I2C Wrapper Functions ---
int8_t Bmp5SensorBase::i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr) {
    hal::I2cLockGuard lock(100);
    if (!lock.acquired()) return BMP5_E_COM_FAIL;

    uint8_t dev_addr = *(uint8_t*)intf_ptr;
    
    Wire.beginTransmission(dev_addr);
    Wire.write(reg_addr);
    if (Wire.endTransmission(false) != 0) { // Repeated Start
        return BMP5_E_COM_FAIL;
    }
    
    Wire.requestFrom((uint8_t)dev_addr, (size_t)length);
    for (uint32_t i = 0; i < length; i++) {
        reg_data[i] = Wire.read();
    }
    return BMP5_OK;
}

int8_t Bmp5SensorBase::i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr) {
    hal::I2cLockGuard lock(100);
    if (!lock.acquired()) return BMP5_E_COM_FAIL;

    uint8_t dev_addr = *(uint8_t*)intf_ptr;
    
    Wire.beginTransmission(dev_addr);
    Wire.write(reg_addr);
    for (uint32_t i = 0; i < length; i++) {
        Wire.write(reg_data[i]);
    }
    if (Wire.endTransmission() != 0) {
        return BMP5_E_COM_FAIL;
    }
    return BMP5_OK;
}

void Bmp5SensorBase::delay_us(uint32_t period, void *intf_ptr) {
    (void)intf_ptr;
    if (period >= 1000) {
        delay(period / 1000);
    } else {
        delayMicroseconds(period);
    }
}

void Bmp5SensorBase::changeState(core::DeviceState newState) {
    if (state_ != newState) {
        state_ = newState;
    }
}

bool Bmp5SensorBase::begin() {
    services::Logger::info(getSensorName(), "Initializing %s (Bosch API)...", getSensorName());
    changeState(core::DeviceState::Initializing);
    
    delay(50);
    
    dev_addr_ = getI2cAddress(); 
    
    bmp5_dev_.intf = BMP5_I2C_INTF;
    bmp5_dev_.intf_ptr = &dev_addr_;
    bmp5_dev_.read = i2c_read;
    bmp5_dev_.write = i2c_write;
    bmp5_dev_.delay_us = delay_us;
    
    bmp5_soft_reset(&bmp5_dev_);
    delay(20);
    
    // Test CHIP ID early
    uint8_t chip_id = 0;
    if (bmp5_get_regs(BMP5_REG_CHIP_ID, &chip_id, 1, &bmp5_dev_) != BMP5_OK) {
        services::Logger::error(getSensorName(), "Failed to communicate on I2C address 0x%02X", dev_addr_);
        changeState(core::DeviceState::Error);
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }
    if (chip_id != getExpectedChipId()) {
        services::Logger::error(getSensorName(), "Chip ID mismatch. Expected 0x%02X, got 0x%02X", getExpectedChipId(), chip_id);
        // Continue anyway in case Bosch API handles it, but log error
    }
    
    int8_t rslt = BMP5_E_COM_FAIL;
    for (int i = 0; i < 5; i++) {
        rslt = bmp5_init(&bmp5_dev_);
        if (rslt == BMP5_OK) {
            break;
        }
        services::Logger::warn(getSensorName(), "Init failed (%d), retrying...", rslt);
        delay(100);
    }

    if (rslt != BMP5_OK) {
        services::Logger::error(getSensorName(), "Failed to init after retries. Error: %d", rslt);
        changeState(core::DeviceState::Error);
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    rslt = bmp5_get_osr_odr_press_config(&osr_odr_press_cfg_, &bmp5_dev_);
    if (rslt == BMP5_OK) {
        osr_odr_press_cfg_.osr_t = BMP5_OVERSAMPLING_2X;
        osr_odr_press_cfg_.osr_p = BMP5_OVERSAMPLING_32X;
        osr_odr_press_cfg_.odr = BMP5_ODR_10_HZ;
        osr_odr_press_cfg_.press_en = BMP5_ENABLE;
        rslt = bmp5_set_osr_odr_press_config(&osr_odr_press_cfg_, &bmp5_dev_);
    }

    if (rslt != BMP5_OK) {
        services::Logger::error(getSensorName(), "Failed to config. Error: %d", rslt);
        changeState(core::DeviceState::Error);
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    rslt = bmp5_set_power_mode(BMP5_POWERMODE_NORMAL, &bmp5_dev_);
    if (rslt != BMP5_OK) {
        services::Logger::error(getSensorName(), "Failed to set power mode. Error: %d", rslt);
        changeState(core::DeviceState::Error);
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    services::Logger::info(getSensorName(), "%s initialized successfully.", getSensorName());
    changeState(core::DeviceState::Ready);
    lastError_ = core::ErrorCode::None;
    consecutiveErrors_ = 0;
    return true;
}

bool Bmp5SensorBase::reinitSensor(uint32_t nowMs) {
    if (nowMs < nextReinitMs_) {
        return false;
    }
    
    services::Logger::warn(getSensorName(), "REINIT_BEGIN: Executing soft reset and re-initialization...");
    resetCount_++;
    
    bmp5_soft_reset(&bmp5_dev_);
    delay(20);
    
    int8_t rslt = bmp5_init(&bmp5_dev_);
    
    services::Logger::info(getSensorName(), "REINIT_END: init result = %d", rslt);
    
    if (rslt != BMP5_OK) {
        if (resetCount_ == 1) {
            nextReinitMs_ = nowMs + 10000;
        } else if (resetCount_ == 2) {
            nextReinitMs_ = nowMs + 30000;
        } else {
            nextReinitMs_ = nowMs + 300000;
        }
        services::Logger::warn(getSensorName(), "Re-init failed. Backing off until %u ms", nextReinitMs_);
        changeState(core::DeviceState::RetryWait);
        return false;
    }

    rslt = bmp5_set_osr_odr_press_config(&osr_odr_press_cfg_, &bmp5_dev_);
    if (rslt != BMP5_OK) {
        nextReinitMs_ = nowMs + 300000;
        changeState(core::DeviceState::RetryWait);
        return false;
    }

    rslt = bmp5_set_power_mode(BMP5_POWERMODE_NORMAL, &bmp5_dev_);
    if (rslt != BMP5_OK) {
        nextReinitMs_ = nowMs + 300000;
        changeState(core::DeviceState::RetryWait);
        return false;
    }
    
    nextReinitMs_ = 0;
    changeState(core::DeviceState::Ready);
    return true;
}

void Bmp5SensorBase::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }
    
    if (state_ == core::DeviceState::RetryWait) {
        if (nowMs >= nextReinitMs_) {
            if (reinitSensor(nowMs)) {
                services::Logger::info(getSensorName(), "Sensor recovered successfully after re-init");
                consecutiveErrors_ = 0;
                resetCount_ = 0;
            }
        }
        return;
    }

    struct bmp5_sensor_data sensor_data;
    int8_t rslt = bmp5_get_sensor_data(&sensor_data, &osr_odr_press_cfg_, &bmp5_dev_);
    float rawPressurePa = sensor_data.pressure;
    float rawTemperatureC = sensor_data.temperature;

    bool validReading = true;
    float pressureHpa = NAN;
    
    if (rslt != BMP5_OK) {
        validReading = false;
    } else {
        pressureHpa = rawPressurePa / 100.0f;
        if (!std::isfinite(pressureHpa) || !std::isfinite(rawTemperatureC) ||
            pressureHpa < 300.0f || pressureHpa > 1250.0f) {
            validReading = false;
        }
    }

    if (!validReading) {
        consecutiveErrors_++;
        isStale_ = true;
        
        services::Logger::warn(getSensorName(), "ts=%u read=NG error=%d raw_pressure=%.1fPa pressure=%.2fhPa invalid_count=%u reset_count=%u",
            nowMs, rslt, rawPressurePa, pressureHpa, consecutiveErrors_, resetCount_);
            
        if (consecutiveErrors_ >= 3) {
            changeState(core::DeviceState::Warning);
            if (reinitSensor(nowMs)) {
                services::Logger::info(getSensorName(), "Sensor recovered successfully after re-init");
                consecutiveErrors_ = 0;
                resetCount_ = 0;
            } else {
                changeState(core::DeviceState::RetryWait);
            }
        }
        return;
    }

    consecutiveErrors_ = 0;
    isStale_ = false;
    currentPressureHpa_ = pressureHpa;
    currentTemperatureC_ = rawTemperatureC;
    
    // 気温を考慮した高度計算: h = ((T + 273.15) / 0.0065) * (1 - (P / P0)^0.190295)
    if (seaLevelPressureHpa_ > 0) {
        currentAltitudeM_ = ((rawTemperatureC + 273.15f) / 0.0065f) * (1.0f - std::pow(pressureHpa / seaLevelPressureHpa_, 0.190295f));
    } else {
        currentAltitudeM_ = NAN;
    }
    
    hasValidData_ = true;
    lastSuccessMs_ = nowMs;
    lastError_ = core::ErrorCode::None;
    changeState(core::DeviceState::Ready);
    
    services::Logger::info(getSensorName(), "ts=%u read=OK raw_pressure=%.1fPa pressure=%.2fhPa temp=%.2fC invalid_count=%u reset_count=%u",
        nowMs, rawPressurePa, pressureHpa, rawTemperatureC, consecutiveErrors_, resetCount_);
}

bool Bmp5SensorBase::readEnvironment(core::EnvironmentData& out) const {
    if (!hasValidData_) {
        out.pressureValid = false;
        return false;
    }
    
    out.pressureHpa = currentPressureHpa_;
    out.pressureValid = !isStale_;
    out.pressureStale = isStale_;
    
    out.altitudeM = currentAltitudeM_;
    out.altitudeValid = (!isStale_ && !std::isnan(currentAltitudeM_));
    
    if (lastSuccessMs_ > out.timestampMs) {
        out.timestampMs = lastSuccessMs_;
    }
    out.valid = true;
    return true;
}

void Bmp5SensorBase::runDiagnostics() {
    Bmp5Diagnostic::runDiagnostics(&bmp5_dev_);
}

} // namespace sensors
} // namespace drivers
