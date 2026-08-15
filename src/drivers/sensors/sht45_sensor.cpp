#include "drivers/sensors/sht45_sensor.h"
#include "services/logger.h"
#include <Wire.h>
#include "hal/clock.h"
#include "hal/i2c_bus.h"

namespace drivers {
namespace sensors {

Sht45Sensor::Sht45Sensor() {}

bool Sht45Sensor::begin() {
    services::Logger::info("SHT45", "Initializing SHT45...");
    state_ = core::DeviceState::Initializing;
    
    sht4x_.begin(Wire, 0x44);

    uint32_t serialNumber;
    uint16_t error = sht4x_.serialNumber(serialNumber);
    if (error) {
        services::Logger::error("SHT45", "Failed to read serial number");
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    services::Logger::info("SHT45", "SHT45 initialized. Serial: %lu", serialNumber);
    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    return true;
}

bool Sht45Sensor::triggerHeater(HeaterPower power, HeaterDuration duration) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return false;
    }

    uint16_t error = 0;
    float tempC = 0.0f;
    float humidityRh = 0.0f;

    hal::I2cLockGuard lock(1500); // Heater commands can take up to 1 second
    if (!lock.acquired()) {
        services::Logger::error("SHT45", "Heater: Failed to acquire lock");
        return false;
    }

    if (power == HeaterPower::Highest && duration == HeaterDuration::Long) {
        error = sht4x_.activateHighestHeaterPowerLong(tempC, humidityRh);
    } else if (power == HeaterPower::Highest && duration == HeaterDuration::Short) {
        error = sht4x_.activateHighestHeaterPowerShort(tempC, humidityRh);
    } else if (power == HeaterPower::Medium && duration == HeaterDuration::Long) {
        error = sht4x_.activateMediumHeaterPowerLong(tempC, humidityRh);
    } else if (power == HeaterPower::Medium && duration == HeaterDuration::Short) {
        error = sht4x_.activateMediumHeaterPowerShort(tempC, humidityRh);
    } else if (power == HeaterPower::Lowest && duration == HeaterDuration::Long) {
        error = sht4x_.activateLowestHeaterPowerLong(tempC, humidityRh);
    } else if (power == HeaterPower::Lowest && duration == HeaterDuration::Short) {
        error = sht4x_.activateLowestHeaterPowerShort(tempC, humidityRh);
    } else {
        return false;
    }

    if (error) {
        char errorMessage[256];
        errorToString(error, errorMessage, 256);
        services::Logger::error("SHT45", "Heater activation failed: %s", errorMessage);
        return false;
    }

    services::Logger::info("SHT45", "Heater activated. Temp after heater: %.2fC, RH: %.2f%%", tempC, humidityRh);
    
    // ヒーターの熱が引くまでクールダウン時間を設ける (例: 15秒)
    preHeaterTempC_ = currentTemperature_;
    preHeaterHumRh_ = currentHumidity_;
    heaterCooldownUntilMs_ = millis() + 15000;
    
    return true;
}

void Sht45Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return; // 後ほど再初期化ロジックを追加可能
    }

    float temperature = 0;
    float humidity = 0;
    
    // 高精度モードでの測定
    uint16_t error = sht4x_.measureHighPrecision(temperature, humidity);
    if (error) {
        services::Logger::warn("SHT45", "Failed to read measurement");
        lastError_ = core::ErrorCode::ReadFailed;
        hasValidData_ = false;
        // 単発エラーなら Degraded にする等の処理が可能ですが、今回はReadyを維持しつつエラー記録
    } else {
        currentTemperature_ = temperature;
        currentHumidity_ = humidity;
        hasValidData_ = true;
        lastSuccessMs_ = nowMs;
        lastError_ = core::ErrorCode::None;
    }
}

bool Sht45Sensor::readEnvironment(core::EnvironmentData& out) const {
    if (!hasValidData_) return false;
    
    uint32_t now = millis();
    if (now < heaterCooldownUntilMs_) {
        // ヒーターのクールダウン中（熱が残っている間）はヒーター直前の正常値を返す
        out.temperatureC = preHeaterTempC_;
        out.humidityRh = preHeaterHumRh_;
    } else {
        out.temperatureC = currentTemperature_;
        out.humidityRh = currentHumidity_;
    }
    
    out.timestampMs = lastSuccessMs_;
    out.valid = true;
    return true;
}

} // namespace sensors
} // namespace drivers
