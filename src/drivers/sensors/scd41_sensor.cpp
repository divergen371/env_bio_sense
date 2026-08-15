#include "drivers/sensors/scd41_sensor.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include <Wire.h>
#include <cmath>

namespace drivers {
namespace sensors {

Scd41Sensor::Scd41Sensor() {}

bool Scd41Sensor::begin() {
    services::Logger::info("SCD41", "Initializing SCD41...");
    state_ = core::DeviceState::Initializing;
    
    scd4x_.begin(Wire);

    uint16_t error = 0;
    char errorMessage[256];

    // SCD41 は Periodic Measurement 実行中は他のコマンドを受け付けない場合があるため、
    // 念のため一度停止してから初期化する
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            error = scd4x_.stopPeriodicMeasurement();
        } else {
            services::Logger::error("SCD41", "Failed to acquire lock for stopPeriodicMeasurement");
            return false;
        }
    }
    
    if (error) {
        services::Logger::warn("SCD41", "stopPeriodicMeasurement failed (might be ok if already stopped)");
    }
    
    delay(500); // 停止コマンド後の待機

    // センサーのシリアルナンバーを取得して通信確認
    uint16_t serial0, serial1, serial2;
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            error = scd4x_.getSerialNumber(serial0, serial1, serial2);
        } else {
            services::Logger::error("SCD41", "Failed to acquire lock for getSerialNumber");
            return false;
        }
    }
    
    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "Failed to get serial: %s", errorMessage);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    services::Logger::info("SCD41", "SCD41 initialized. Serial: 0x%04X%04X%04X", serial0, serial1, serial2);

    // ASC (Automatic Self-Calibration) の無効化と、筐体内の自己発熱を考慮した温度オフセット(2.0℃)の設定
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            error = scd4x_.setAutomaticSelfCalibration(0);
            if (error) {
                services::Logger::warn("SCD41", "Failed to disable ASC, error: %u", error);
            }
            error = scd4x_.setTemperatureOffset(2.0f);
            if (error) {
                services::Logger::warn("SCD41", "Failed to set temperature offset, error: %u", error);
            }
            
            // ユーザー確認用のレジスタ読み出し
            float tOffset = 0.0f;
            uint16_t ascEnabled = 0;
            uint16_t sensorAlt = 0;
            scd4x_.getTemperatureOffset(tOffset);
            scd4x_.getAutomaticSelfCalibration(ascEnabled);
            scd4x_.getSensorAltitude(sensorAlt);
            services::Logger::info("SCD41", "Stored Settings - TempOffset: %.2f C, ASC: %u, Altitude: %u m", tOffset, ascEnabled, sensorAlt);
        } else {
            services::Logger::error("SCD41", "Failed to acquire lock for SCD41 settings");
        }
    }

    // Periodic Measurement 開始 (測定間隔は約5秒)
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            error = scd4x_.startPeriodicMeasurement();
        } else {
            services::Logger::error("SCD41", "Failed to acquire lock for startPeriodicMeasurement");
            return false;
        }
    }

    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "Failed to start periodic measurement: %s", errorMessage);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    successCount_ = 0;
    readErrorCount_ = 0;
    notReadyCount_ = 0;
    consecutiveErrors_ = 0;
    return true;
}

bool Scd41Sensor::performManualCalibration(uint16_t targetCo2Ppm, uint16_t& frcCorrection) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        services::Logger::error("SCD41", "Cannot perform FRC in current state");
        return false;
    }

    uint16_t error = 0;
    char errorMessage[256];

    // 1. Stop periodic measurement
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            error = scd4x_.stopPeriodicMeasurement();
        } else {
            services::Logger::error("SCD41", "FRC: Failed to lock for stopPeriodicMeasurement");
            return false;
        }
    }

    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::warn("SCD41", "FRC: stopPeriodicMeasurement failed: %s", errorMessage);
    }

    // 2. Wait 500ms
    delay(500);

    // 3. Perform FRC
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            error = scd4x_.performForcedRecalibration(targetCo2Ppm, frcCorrection);
        } else {
            services::Logger::error("SCD41", "FRC: Failed to lock for performForcedRecalibration");
            return false;
        }
    }

    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "FRC failed: %s", errorMessage);
    } else if (frcCorrection == 0xFFFF) {
        services::Logger::error("SCD41", "FRC failed: 0xFFFF returned");
        error = 1;
    } else {
        services::Logger::info("SCD41", "FRC success. Correction: 0x%04X", frcCorrection);
    }

    // 4. Restart periodic measurement
    uint16_t restartError = 0;
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            restartError = scd4x_.startPeriodicMeasurement();
        } else {
            services::Logger::error("SCD41", "FRC: Failed to lock for startPeriodicMeasurement");
        }
    }
    
    if (restartError) {
        services::Logger::error("SCD41", "FRC: startPeriodicMeasurement failed");
        state_ = core::DeviceState::Error;
    }

    return error == 0;
}

void Scd41Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }

    // 最新のデータがないかポーリング (約5秒ごとに準備完了になる)
    bool isDataReady = false;
    uint16_t error = 0;
    
    {
        hal::I2cLockGuard lock(100);
        if (!lock.acquired()) {
            services::Logger::warn("SCD41", "SCD41 ts=%u lock=TIMEOUT ready=? read=SKIPPED", nowMs);
            return;
        }
        error = scd4x_.getDataReadyFlag(isDataReady);
    }
    
    if (error) {
        readErrorCount_++;
        consecutiveErrors_++;
        services::Logger::warn("SCD41", "SCD41 ts=%u lock=OK ready=ERR error=%d read=SKIPPED", nowMs, error);
        return;
    }

    if (!isDataReady) {
        notReadyCount_++;
        return; // データはまだない
    }

    // データ読み取り
    uint16_t co2 = 0;
    float temperature = NAN;
    float humidity = NAN;
    
    {
        hal::I2cLockGuard lock(100);
        if (!lock.acquired()) {
            services::Logger::warn("SCD41", "SCD41 ts=%u lock=TIMEOUT ready=1 read=SKIPPED", nowMs);
            return;
        }
        error = scd4x_.readMeasurement(co2, temperature, humidity);
    }
    
    if (error || co2 == 0 || !std::isfinite(temperature) || !std::isfinite(humidity)) {
        readErrorCount_++;
        consecutiveErrors_++;
        hasValidData_ = false; // 無効値の場合は最新データを保持しない (stale状態にする)
        lastError_ = core::ErrorCode::ReadFailed;
        services::Logger::warn("SCD41", "SCD41 ts=%u lock=OK ready=1 read=NG error=%d co2=%u temp=%.2f rh=%.2f (consec_err=%u)",
            nowMs, error, co2, temperature, humidity, consecutiveErrors_);
        return;
    }

    // 正常データの保存
    currentCo2Ppm_ = co2;
    currentTemperature_ = temperature;
    currentHumidity_ = humidity;
    
    successCount_++;
    consecutiveErrors_ = 0;
    hasValidData_ = true;
    lastSuccessMs_ = nowMs;
    lastError_ = core::ErrorCode::None;
    
    services::Logger::info("SCD41", "SCD41 ts=%u lock=OK ready=1 read=OK co2=%u temp=%.2f rh=%.2f crc=OK succ=%u err=%u nr=%u",
        nowMs, co2, temperature, humidity, successCount_, readErrorCount_, notReadyCount_);
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

void Scd41Sensor::setAmbientPressure(uint16_t pressureHpa) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }
    
    hal::I2cLockGuard lock(100);
    if (lock.acquired()) {
        uint16_t error = scd4x_.setAmbientPressure(pressureHpa);
        if (error) {
            services::Logger::warn("SCD41", "Failed to set ambient pressure: %u hPa", pressureHpa);
        }
    }
}

} // namespace sensors
} // namespace drivers
