#include "drivers/sensors/scd41_sensor.h"
#include "services/logger.h"
#include <Wire.h>

namespace drivers {
namespace sensors {

Scd41Sensor::Scd41Sensor() {}

bool Scd41Sensor::begin() {
    services::Logger::info("SCD41", "Initializing SCD41...");
    state_ = core::DeviceState::Initializing;
    
    scd4x_.begin(Wire);

    uint16_t error;
    char errorMessage[256];

    // SCD41 は Periodic Measurement 実行中は他のコマンドを受け付けない場合があるため、
    // 念のため一度停止してから初期化する
    error = scd4x_.stopPeriodicMeasurement();
    if (error) {
        services::Logger::warn("SCD41", "stopPeriodicMeasurement failed (might be ok if already stopped)");
    }
    
    delay(500); // 停止コマンド後の待機

    // センサーのシリアルナンバーを取得して通信確認
    uint16_t serial0, serial1, serial2;
    error = scd4x_.getSerialNumber(serial0, serial1, serial2);
    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "Failed to get serial: %s", errorMessage);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    services::Logger::info("SCD41", "SCD41 initialized. Serial: 0x%04X%04X%04X", serial0, serial1, serial2);

    // Periodic Measurement 開始 (測定間隔は約5秒)
    error = scd4x_.startPeriodicMeasurement();
    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "Failed to start periodic measurement: %s", errorMessage);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    return true;
}

void Scd41Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }

    // 最新のデータがないかポーリング (約5秒ごとに準備完了になる)
    bool isDataReady = false;
    uint16_t error = scd4x_.getDataReadyFlag(isDataReady);
    
    if (error) {
        // 通信エラー等
        services::Logger::warn("SCD41", "Failed to check data ready flag");
        return;
    }

    if (!isDataReady) {
        return; // データはまだない
    }

    // データ読み取り
    uint16_t co2;
    float temperature;
    float humidity;
    error = scd4x_.readMeasurement(co2, temperature, humidity);
    
    if (error) {
        services::Logger::warn("SCD41", "Failed to read measurement");
        lastError_ = core::ErrorCode::ReadFailed;
        return;
    }

    if (co2 == 0) {
        // CO2 が 0 は無効なデータ
        services::Logger::warn("SCD41", "Invalid measurement (CO2=0)");
        return;
    }

    // データの保存（温度と湿度も取得して熱ごもり検知に使用する）
    currentCo2Ppm_ = co2;
    currentTemperature_ = temperature;
    currentHumidity_ = humidity;
    
    hasValidData_ = true;
    lastSuccessMs_ = nowMs;
    lastError_ = core::ErrorCode::None;
}

bool Scd41Sensor::readEnvironment(core::EnvironmentData& out) const {
    if (!hasValidData_) return false;
    
    out.co2Ppm = currentCo2Ppm_;
    out.scd41TemperatureC = currentTemperature_;
    out.scd41HumidityRh = currentHumidity_;
    
    if (lastSuccessMs_ > out.timestampMs) {
        out.timestampMs = lastSuccessMs_;
    }
    
    out.valid = true;
    return true;
}

} // namespace sensors
} // namespace drivers
