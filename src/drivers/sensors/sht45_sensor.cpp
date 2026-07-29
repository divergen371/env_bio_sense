#include "drivers/sensors/sht45_sensor.h"
#include "services/logger.h"
#include <Wire.h>

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
    
    out.temperatureC = currentTemperature_;
    out.humidityRh = currentHumidity_;
    out.timestampMs = lastSuccessMs_;
    out.valid = true;
    return true;
}

} // namespace sensors
} // namespace drivers
