#include "drivers/sensors/bmp585_sensor.h"
#include "services/logger.h"
#include <Wire.h>

namespace drivers {
namespace sensors {

Bmp585Sensor::Bmp585Sensor() {}

bool Bmp585Sensor::begin() {
    services::Logger::info("BMP585", "Initializing BMP585...");
    state_ = core::DeviceState::Initializing;
    
    // I2C address for BMP585 is usually 0x46 or 0x47. 
    // Adafruit library defaults to 0x47, but our I2C scanner showed 0x46.
    if (!bmp_.begin(0x46, &Wire)) {
        services::Logger::error("BMP585", "Failed to find BMP585 at 0x46");
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    // デフォルトの設定を使用
    // bmp_.setTemperatureOversampling(BMP5_OVERSAMPLING_8X);
    // bmp_.setPressureOversampling(BMP5_OVERSAMPLING_128X);
    // bmp_.setIIRFilterCoeff(BMP5_IIR_FILTER_COEFF_3);
    // bmp_.setOutputDataRate(BMP5_ODR_10_HZ);

    services::Logger::info("BMP585", "BMP585 initialized.");
    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    return true;
}

void Bmp585Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }

    if (!bmp_.performReading()) {
        services::Logger::warn("BMP585", "Failed to perform reading");
        lastError_ = core::ErrorCode::ReadFailed;
        hasValidData_ = false;
        return;
    }

    // bmp_.pressure は hPa単位、bmp_.temperature は Celsius
    currentPressureHpa_ = bmp_.pressure;
    hasValidData_ = true;
    lastSuccessMs_ = nowMs;
    lastError_ = core::ErrorCode::None;
}

bool Bmp585Sensor::readEnvironment(core::EnvironmentData& out) const {
    if (!hasValidData_) return false;
    
    // 温度は SHT45 側を正とするため、ここでは気圧のみ更新
    out.pressureHpa = currentPressureHpa_;
    // タイムスタンプは新しい方とするか単純に更新
    if (lastSuccessMs_ > out.timestampMs) {
        out.timestampMs = lastSuccessMs_;
    }
    out.valid = true;
    return true;
}

} // namespace sensors
} // namespace drivers
