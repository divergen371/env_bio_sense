#include "drivers/sensors/bme690_sensor.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include <Arduino.h>
#include <Wire.h>
#include <cmath>

namespace drivers {
namespace sensors {

static constexpr uint8_t BME690_I2C_ADDR = 0x77;

BME69X_INTF_RET_TYPE bme690_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr) {
    hal::I2cLockGuard lock(hal::I2cDevice::Bme690,
                           hal::I2cOperation::Read, 50);
    if (!lock.acquired()) {
        return BME69X_E_COM_FAIL;
    }

    Wire.beginTransmission(BME690_I2C_ADDR);
    Wire.write(reg_addr);
    if (Wire.endTransmission() != 0) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Bme690,
                                            hal::I2cOperation::Read);
        return BME69X_E_COM_FAIL;
    }

    uint8_t bytesReceived = Wire.requestFrom((uint16_t)BME690_I2C_ADDR, (uint8_t)len, true);
    if (bytesReceived != len) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Bme690,
                                            hal::I2cOperation::Read);
        while (Wire.available() > 0) Wire.read();
        return BME69X_E_COM_FAIL;
    }

    for (uint32_t i = 0; i < len; i++) {
        if (Wire.available()) {
            reg_data[i] = Wire.read();
        } else {
            hal::I2cBus::noteCommunicationError(hal::I2cDevice::Bme690,
                                                hal::I2cOperation::Read);
            return BME69X_E_COM_FAIL;
        }
    }

    return BME69X_OK;
}

BME69X_INTF_RET_TYPE bme690_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr) {
    hal::I2cLockGuard lock(hal::I2cDevice::Bme690,
                           hal::I2cOperation::Write, 50);
    if (!lock.acquired()) {
        return BME69X_E_COM_FAIL;
    }

    Wire.beginTransmission(BME690_I2C_ADDR);
    Wire.write(reg_addr);
    for (uint32_t i = 0; i < len; i++) {
        Wire.write(reg_data[i]);
    }
    if (Wire.endTransmission() != 0) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Bme690,
                                            hal::I2cOperation::Write);
        return BME69X_E_COM_FAIL;
    }

    return BME69X_OK;
}

void bme690_delay_us(uint32_t period, void *intf_ptr) {
    delayMicroseconds(period);
}

Bme690Sensor::Bme690Sensor() {
    bmeDev_.intf = BME69X_I2C_INTF;
    bmeDev_.read = bme690_i2c_read;
    bmeDev_.write = bme690_i2c_write;
    bmeDev_.delay_us = bme690_delay_us;
    bmeDev_.intf_ptr = nullptr;
    bmeDev_.amb_temp = 25;
}

bool Bme690Sensor::begin() {
    services::Logger::info("Bme690", "Initializing BME690...");
    if (initDevice()) {
        state_ = core::DeviceState::Ready;
        lastError_ = core::ErrorCode::None;
        return true;
    } else {
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        lastData_.tphValid = false;
        lastData_.gasValid = false;
        return false;
    }
}

bool Bme690Sensor::initDevice() {
    int8_t rslt = bme69x_init(&bmeDev_);
    if (rslt != BME69X_OK) {
        services::Logger::error("Bme690", "bme69x_init failed: %d", rslt);
        return false;
    }
    
    // Check variant ID
    if (bmeDev_.variant_id != BME69X_VARIANT_GAS_LOW) {
        services::Logger::warn("Bme690", "Variant ID mismatch: 0x%02X, but proceeding.", bmeDev_.variant_id);
    }

    services::Logger::info("Bme690", "BME690 Chip ID: 0x%02X, Variant ID: 0x%02X", bmeDev_.chip_id, bmeDev_.variant_id);

    // Initial configuration for forced mode
    bmeConf_.filter = BME69X_FILTER_OFF;
    bmeConf_.odr = BME69X_ODR_NONE;
    bmeConf_.os_hum = BME69X_OS_16X;
    bmeConf_.os_pres = BME69X_OS_16X;
    bmeConf_.os_temp = BME69X_OS_16X;

    rslt = bme69x_set_conf(&bmeConf_, &bmeDev_);
    if (rslt != BME69X_OK) {
        services::Logger::error("Bme690", "bme69x_set_conf failed: %d", rslt);
        return false;
    }

    heatrConf_.enable = BME69X_ENABLE;
    heatrConf_.heatr_temp = 300;
    heatrConf_.heatr_dur = 100;

    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatrConf_, &bmeDev_);
    if (rslt != BME69X_OK) {
        services::Logger::error("Bme690", "bme69x_set_heatr_conf failed: %d", rslt);
        return false;
    }

    // Calculate measurement delay
    measureDelayUs_ = bme69x_get_meas_dur(BME69X_FORCED_MODE, &bmeConf_, &bmeDev_) + (heatrConf_.heatr_dur * 1000);
    services::Logger::info("Bme690", "Measurement delay: %lu us", measureDelayUs_);

    errorCount_ = 0;
    lastData_ = core::Bme690Data{};
    machineState_ = State::Idle;
    lastMeasureTriggerMs_ = 0;

    return true;
}

void Bme690Sensor::setError(core::ErrorCode err) {
    lastError_ = err;
    lastData_.tphValid = false;
    lastData_.gasValid = false;
    errorCount_++;
    if (errorCount_ > 5) {
        state_ = core::DeviceState::Offline;
    } else {
        state_ = core::DeviceState::Warning;
    }
}

void Bme690Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        // Retry initialization periodically (e.g. every 60 seconds)
        if (nowMs - lastReinitMs_ > 60000) {
            lastReinitMs_ = nowMs;
            if (initDevice()) {
                state_ = core::DeviceState::Ready;
                lastError_ = core::ErrorCode::None;
                services::Logger::info("Bme690", "Recovered from offline state");
            }
        }
        return;
    }

    if (machineState_ == State::Idle) {
        // Trigger measurement every 5 seconds
        if (nowMs - lastMeasureTriggerMs_ >= 5000) {
            if (triggerMeasurement()) {
                lastMeasureTriggerMs_ = nowMs;
                machineState_ = State::Waiting;
            }
        }
    } else if (machineState_ == State::Waiting) {
        uint32_t elapsedMs = nowMs - lastMeasureTriggerMs_;
        uint32_t requiredMs = (measureDelayUs_ / 1000) + 1; // Round up
        
        // Wait a little extra to be safe (e.g., +2ms)
        if (elapsedMs > requiredMs + 2) {
            readMeasurement();
            machineState_ = State::Idle;
        }
    }
}

bool Bme690Sensor::triggerMeasurement() {
    int8_t rslt = bme69x_set_op_mode(BME69X_FORCED_MODE, &bmeDev_);
    if (rslt != BME69X_OK) {
        setError(core::ErrorCode::BusError);
        return false;
    }
    return true;
}

void Bme690Sensor::readMeasurement() {
    struct bme69x_data data[3];
    uint8_t n_data = 0;

    int8_t rslt = bme69x_get_data(BME69X_FORCED_MODE, data, &n_data, &bmeDev_);
    if (rslt != BME69X_OK) {
        setError(core::ErrorCode::ReadFailed);
        return;
    }

    if (n_data == 0) {
        // No new data
        return;
    }

    // Extract latest data (index 0 for forced mode)
    const struct bme69x_data& d = data[0];

    // Filter valid data ranges
    bool tphValid = false;
    if (d.status & BME69X_NEW_DATA_MSK) {
        if (std::isfinite(d.temperature) && std::isfinite(d.humidity) && std::isfinite(d.pressure)) {
            if (d.temperature >= -40.0f && d.temperature <= 85.0f &&
                d.humidity >= 0.0f && d.humidity <= 100.0f &&
                (d.pressure / 100.0f) >= 300.0f && (d.pressure / 100.0f) <= 1100.0f) {
                
                tphValid = true;
                lastData_.temperatureC = d.temperature;
                lastData_.humidityRh = d.humidity;
                lastData_.pressureHpa = d.pressure / 100.0f; // Pa to hPa
            }
        }
    }

    bool gasValid = false;
    if ((d.status & BME69X_NEW_DATA_MSK) && 
        (d.status & BME69X_GASM_VALID_MSK) && 
        (d.status & BME69X_HEAT_STAB_MSK)) {
        
        if (std::isfinite(d.gas_resistance) && d.gas_resistance > 0.0f) {
            gasValid = true;
            lastData_.gasResistanceOhm = d.gas_resistance;
        }
    }
    
    if (!gasValid) {
        lastData_.gasResistanceOhm = NAN;
    }

    lastData_.gasIndex = d.gas_index;
    lastData_.status = d.status;
    lastData_.tphValid = tphValid;
    lastData_.gasValid = gasValid;
    lastData_.heaterStable = (d.status & BME69X_HEAT_STAB_MSK) != 0;

    if (tphValid) {
        lastData_.timestampMs = millis();
        lastSuccessMs_ = lastData_.timestampMs;
        errorCount_ = 0;
        state_ = core::DeviceState::Ready;
        lastError_ = core::ErrorCode::None;
    }
}

bool Bme690Sensor::readData(core::Bme690Data& out) const {
    const bool stateReadable = state_ == core::DeviceState::Ready ||
                               state_ == core::DeviceState::Warning;
    if (stateReadable && lastSuccessMs_ != 0 &&
        millis() - lastSuccessMs_ <= DATA_MAX_AGE_MS &&
        lastData_.tphValid) {
        out = lastData_;
        return true;
    }
    out = lastData_;
    out.tphValid = false;
    out.gasValid = false;
    return false;
}

} // namespace sensors
} // namespace drivers
