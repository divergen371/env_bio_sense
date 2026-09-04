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
    lastError_ = core::ErrorCode::None;
    condition_ = Scd41Condition::AwaitingFirstSample;
    lastRawError_ = 0;
    hasValidData_ = false;
    lastSuccessMs_ = 0;
    recoveryPhase_ = RecoveryPhase::Idle;
    measurementStartMs_ = millis();
    nextRecoveryAttemptMs_ = measurementStartMs_ + 1000;
    
    scd4x_.begin(Wire);

    uint16_t error = 0;
    char errorMessage[256];

    // SCD41 は Periodic Measurement 実行中は他のコマンドを受け付けない場合があるため、
    // 念のため一度停止してから初期化する
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Init, 100);
        if (lock.acquired()) {
            error = scd4x_.stopPeriodicMeasurement();
        } else {
            services::Logger::error("SCD41", "Failed to acquire lock for stopPeriodicMeasurement");
            state_ = core::DeviceState::Error;
            lastError_ = core::ErrorCode::Timeout;
            condition_ = Scd41Condition::LockTimeout;
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
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Init, 100);
        if (lock.acquired()) {
            error = scd4x_.getSerialNumber(serial0, serial1, serial2);
        } else {
            services::Logger::error("SCD41", "Failed to acquire lock for getSerialNumber");
            state_ = core::DeviceState::Error;
            lastError_ = core::ErrorCode::Timeout;
            condition_ = Scd41Condition::LockTimeout;
            return false;
        }
    }
    
    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "Failed to get serial: %s", errorMessage);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        condition_ = Scd41Condition::DriverError;
        lastRawError_ = error;
        return false;
    }

    services::Logger::info("SCD41", "SCD41 initialized. Serial: 0x%04X%04X%04X", serial0, serial1, serial2);

    // ASC (Automatic Self-Calibration) の無効化と、筐体内の自己発熱を考慮した温度オフセット(2.0℃)の設定
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Init, 100);
        if (lock.acquired()) {
            error = scd4x_.setAutomaticSelfCalibration(1);
            if (error) {
                services::Logger::warn("SCD41", "Failed to enable ASC, error: %u", error);
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
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Init, 100);
        if (lock.acquired()) {
            error = scd4x_.startPeriodicMeasurement();
        } else {
            services::Logger::error("SCD41", "Failed to acquire lock for startPeriodicMeasurement");
            state_ = core::DeviceState::Error;
            lastError_ = core::ErrorCode::Timeout;
            condition_ = Scd41Condition::LockTimeout;
            return false;
        }
    }

    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "Failed to start periodic measurement: %s", errorMessage);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        condition_ = Scd41Condition::DriverError;
        lastRawError_ = error;
        return false;
    }

    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    successCount_ = 0;
    readErrorCount_ = 0;
    notReadyCount_ = 0;
    consecutiveErrors_ = 0;
    measurementStartMs_ = millis();
    nextRecoveryAttemptMs_ = 0;
    condition_ = Scd41Condition::AwaitingFirstSample;
    return true;
}

bool Scd41Sensor::performForcedRecalibration(uint16_t referenceCo2Ppm, Scd41FrcResult& result) {
    result.errorMessage = nullptr;
    if (state_ != core::DeviceState::Ready || condition_ != Scd41Condition::Healthy) {
        services::Logger::error("SCD41", "Cannot perform FRC in current state");
        result.errorMessage = "SENSOR_NOT_READY";
        return false;
    }
    if (calibrationInProgress_) {
        services::Logger::error("SCD41", "Calibration already in progress");
        result.errorMessage = "CALIBRATION_ALREADY_IN_PROGRESS";
        return false;
    }

    uint32_t nowMs = millis();
    uint32_t uptimeMs = nowMs - measurementStartMs_;
    if (measurementStartMs_ == 0 || uptimeMs < 180000) {
        services::Logger::error("SCD41", "FRC rejected: measurement uptime too short (%u ms, need 180000)", uptimeMs);
        result.errorMessage = "MEASUREMENT_UPTIME_TOO_SHORT";
        return false;
    }
    if (!hasValidData_ || lastSuccessMs_ == 0 || nowMs - lastSuccessMs_ > DATA_STALE_MS) {
        services::Logger::error("SCD41", "FRC rejected: no valid CO2 data");
        result.errorMessage = "NO_VALID_DATA";
        return false;
    }
    if (referenceCo2Ppm < 400 || referenceCo2Ppm > 5000) {
        services::Logger::error("SCD41", "FRC rejected: reference ppm out of range (400-5000)");
        result.errorMessage = "REFERENCE_OUT_OF_RANGE";
        return false;
    }
    // Pressure freshness check (e.g. 15 seconds)
    if (hasAmbientPressure_ && (nowMs - lastAmbientPressureSetMs_ > 15000)) {
        services::Logger::error("SCD41", "FRC rejected: ambient pressure is stale (%u ms old)", nowMs - lastAmbientPressureSetMs_);
        result.errorMessage = "PRESSURE_STALE";
        return false;
    }

    services::Logger::info("SCD41", "event=SCD41_FRC_BEGIN reference_ppm=%u pre_co2_ppm=%u ambient_pressure_hpa=%u pressure_age_ms=%u measurement_uptime_ms=%u",
        referenceCo2Ppm, currentCo2Ppm_, hasAmbientPressure_ ? lastAmbientPressureHpa_ : 0, 
        hasAmbientPressure_ ? (nowMs - lastAmbientPressureSetMs_) : 0, uptimeMs);

    calibrationInProgress_ = true;
    uint16_t error = 0;
    char errorMessage[256];

    // 1. Stop periodic measurement
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Maintenance, 100);
        if (lock.acquired()) {
            error = scd4x_.stopPeriodicMeasurement();
        } else {
            services::Logger::error("SCD41", "FRC: Failed to lock for stopPeriodicMeasurement");
            calibrationInProgress_ = false;
            result.errorMessage = "LOCK_FAILED_FOR_STOP";
            return false;
        }
    }

    if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "FRC: stopPeriodicMeasurement failed: %s. Aborting FRC.", errorMessage);
        calibrationInProgress_ = false;
        result.errorMessage = "STOP_MEASUREMENT_FAILED";
        return false;
    }

    // 2. Wait 500ms
    delay(500);

    // 3. Perform FRC
    uint16_t rawFrc = 0;
    bool frcLockAcquired = false;
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Maintenance, 100);
        if (lock.acquired()) {
            frcLockAcquired = true;
            error = scd4x_.performForcedRecalibration(referenceCo2Ppm, rawFrc);
        } else {
            services::Logger::error("SCD41", "FRC: Failed to lock for performForcedRecalibration");
            // The sensor is already stopped. Continue to the common restart
            // path so a transient lock timeout cannot leave it stopped.
            error = 0xFFFE;
        }
    }

    result.referencePpm = referenceCo2Ppm;
    result.preCalibrationCo2Ppm = currentCo2Ppm_;
    result.ambientPressureHpa = hasAmbientPressure_ ? lastAmbientPressureHpa_ : 0;
    result.measurementUptimeMs = uptimeMs;
    result.rawWord = rawFrc;
    result.success = false;

    if (!frcLockAcquired) {
        services::Logger::error("SCD41", "FRC failed: I2C lock timeout");
        result.errorMessage = "LOCK_FAILED_FOR_FRC";
    } else if (error) {
        errorToString(error, errorMessage, 256);
        services::Logger::error("SCD41", "FRC failed: %s", errorMessage);
        result.errorMessage = "FRC_FAILED_COMM_ERROR";
    } else if (rawFrc == 0xFFFF) {
        services::Logger::error("SCD41", "FRC failed: 0xFFFF returned (sensor rejected FRC)");
        result.errorMessage = "FRC_FAILED_SENSOR_REJECTED";
    } else {
        result.correctionPpm = static_cast<int16_t>(rawFrc) - 0x8000;
        result.success = true;
        result.errorMessage = "";
        services::Logger::info("SCD41", "event=SCD41_FRC_RESULT success=true raw_word=0x%04X correction_ppm=%d", rawFrc, result.correctionPpm);
        postFrcLogCount_ = 5; // Log the next 5 measurements
    }

    if (!result.success) {
        services::Logger::warn("SCD41", "event=SCD41_FRC_RESULT success=false raw_word=0x%04X", rawFrc);
    }

    // 4. Restart periodic measurement
    uint16_t restartError = 0;
    bool restartLockAcquired = false;
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Maintenance, 100);
        if (lock.acquired()) {
            restartLockAcquired = true;
            restartError = scd4x_.startPeriodicMeasurement();
        } else {
            services::Logger::error("SCD41", "FRC: Failed to lock for startPeriodicMeasurement");
            restartError = 0xFFFE;
        }
    }
    
    if (restartError) {
        services::Logger::error("SCD41", "FRC: startPeriodicMeasurement failed");
        state_ = core::DeviceState::RetryWait;
        lastError_ = restartLockAcquired ? core::ErrorCode::ReadFailed : core::ErrorCode::Timeout;
        condition_ = Scd41Condition::RecoveryFailed;
        lastRawError_ = restartError;
        hasValidData_ = false;
        nextRecoveryAttemptMs_ = millis() + 1000;
        result.success = false;
        result.errorMessage = "RESTART_MEASUREMENT_FAILED";
    } else {
        measurementStartMs_ = millis();
        lastSuccessMs_ = 0;
        hasValidData_ = false;
        state_ = core::DeviceState::Ready;
        lastError_ = core::ErrorCode::None;
        condition_ = Scd41Condition::AwaitingFirstSample;
        nextRecoveryAttemptMs_ = 0;
    }

    calibrationInProgress_ = false;
    return result.success;
}

bool Scd41Sensor::factoryResetAndReconfigure() {
    if (calibrationInProgress_) return false;

    float beforeOffset = NAN;
    uint16_t beforeAsc = 0;
    
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Maintenance, 100);
        if (lock.acquired()) {
            scd4x_.getTemperatureOffset(beforeOffset);
            scd4x_.getAutomaticSelfCalibration(beforeAsc);
            scd4x_.stopPeriodicMeasurement();
        } else {
            return false;
        }
    }
    
    services::Logger::info("SCD41", "Performing Factory Reset... (Before - TempOffset: %.2f C, ASC: %u)", beforeOffset, beforeAsc);

    uint16_t error = 0;
    delay(500);

    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Maintenance, 100);
        if (lock.acquired()) {
            error = scd4x_.performFactoryReset();
        } else {
            markRecoveryFailure(millis(), Scd41Condition::RecoveryFailed,
                                core::ErrorCode::Timeout, 0xFFFE);
            return false;
        }
    }
    
    delay(1200); // Wait for sensor to boot up after reset
    
    if (error) {
        services::Logger::error("SCD41", "Factory Reset failed");
        state_ = core::DeviceState::RetryWait;
        lastError_ = core::ErrorCode::ReadFailed;
        condition_ = Scd41Condition::RecoveryFailed;
        lastRawError_ = error;
        nextRecoveryAttemptMs_ = millis() + RECOVERY_RETRY_MS;
        return false;
    }
    
    services::Logger::info("SCD41", "Factory Reset successful, reconfiguring...");
    
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Maintenance, 100);
        if (lock.acquired()) {
            scd4x_.setAutomaticSelfCalibration(1);
            scd4x_.setTemperatureOffset(2.0f);
            
            float tOffset = 0.0f;
            uint16_t ascEnabled = 0;
            scd4x_.getTemperatureOffset(tOffset);
            scd4x_.getAutomaticSelfCalibration(ascEnabled);
            services::Logger::info("SCD41", "Reconfigured - TempOffset: %.2f C, ASC: %u", tOffset, ascEnabled);
            
            error = scd4x_.startPeriodicMeasurement();
        } else {
            markRecoveryFailure(millis(), Scd41Condition::RecoveryFailed,
                                core::ErrorCode::Timeout, 0xFFFE);
            return false;
        }
    }
    
    if (error) {
        services::Logger::error("SCD41", "Failed to restart measurement after factory reset");
        state_ = core::DeviceState::RetryWait;
        lastError_ = core::ErrorCode::ReadFailed;
        condition_ = Scd41Condition::RecoveryFailed;
        lastRawError_ = error;
        nextRecoveryAttemptMs_ = millis() + RECOVERY_RETRY_MS;
        return false;
    }
    
    measurementStartMs_ = millis();
    lastSuccessMs_ = 0;
    hasValidData_ = false;
    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    condition_ = Scd41Condition::AwaitingFirstSample;
    nextRecoveryAttemptMs_ = 0;
    return true;
}

void Scd41Sensor::markRecoveryFailure(uint32_t nowMs, Scd41Condition condition,
                                      core::ErrorCode errorCode, uint16_t rawError) {
    recoveryPhase_ = RecoveryPhase::Idle;
    state_ = core::DeviceState::RetryWait;
    lastError_ = errorCode;
    condition_ = condition;
    lastRawError_ = rawError;
    hasValidData_ = false;
    consecutiveErrors_++;
    nextRecoveryAttemptMs_ = nowMs + RECOVERY_RETRY_MS;
}

bool Scd41Sensor::beginRecovery(uint32_t nowMs) {
    hasValidData_ = false;
    state_ = core::DeviceState::Degraded;
    condition_ = Scd41Condition::RecoveryStopping;
    nextRecoveryAttemptMs_ = nowMs + RECOVERY_RETRY_MS;

    uint16_t error = 0;
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Maintenance, 100);
        if (!lock.acquired()) {
            services::Logger::warn("SCD41", "Recovery stop lock timeout");
            markRecoveryFailure(nowMs, Scd41Condition::RecoveryFailed,
                                core::ErrorCode::Timeout, 0xFFFE);
            return false;
        }
        error = scd4x_.stopPeriodicMeasurement();
    }

    if (error) {
        // The sensor may already be stopped (for example after an interrupted
        // FRC). Waiting and attempting start is still the safest recovery path.
        services::Logger::warn("SCD41", "Recovery stop returned %u; continuing with restart", error);
    }

    recoveryPhase_ = RecoveryPhase::WaitAfterStop;
    recoveryDeadlineMs_ = nowMs + STOP_TO_START_DELAY_MS;
    condition_ = Scd41Condition::RecoveryWaiting;
    lastRawError_ = error;
    services::Logger::warn("SCD41", "Recovery initiated after stale/failed measurements");
    return true;
}

void Scd41Sensor::continueRecovery(uint32_t nowMs) {
    if (recoveryPhase_ != RecoveryPhase::WaitAfterStop) return;
    if (static_cast<int32_t>(nowMs - recoveryDeadlineMs_) < 0) return;

    uint16_t error = 0;
    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Maintenance, 100);
        if (!lock.acquired()) {
            services::Logger::warn("SCD41", "Recovery start lock timeout");
            markRecoveryFailure(nowMs, Scd41Condition::RecoveryFailed,
                                core::ErrorCode::Timeout, 0xFFFE);
            return;
        }
        error = scd4x_.startPeriodicMeasurement();
    }

    if (error) {
        services::Logger::warn("SCD41", "Recovery start failed: %u", error);
        markRecoveryFailure(nowMs, Scd41Condition::RecoveryFailed,
                            core::ErrorCode::ReadFailed, error);
        return;
    }

    recoveryPhase_ = RecoveryPhase::Idle;
    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    lastRawError_ = 0;
    hasValidData_ = false;
    lastSuccessMs_ = 0;
    measurementStartMs_ = nowMs;
    consecutiveErrors_ = 0;
    condition_ = Scd41Condition::AwaitingFirstSample;
    services::Logger::info("SCD41", "Periodic measurement restarted; awaiting fresh sample");
}

void Scd41Sensor::update(uint32_t nowMs) {
    if (calibrationInProgress_) return;

    if (recoveryPhase_ != RecoveryPhase::Idle) {
        continueRecovery(nowMs);
        return;
    }

    bool firstSampleTimedOut = lastSuccessMs_ == 0 &&
                               nowMs - measurementStartMs_ > DATA_STALE_MS;
    bool lastSampleStale = lastSuccessMs_ != 0 && nowMs - lastSuccessMs_ > DATA_STALE_MS;
    bool driverUnavailable = state_ == core::DeviceState::Error ||
                             state_ == core::DeviceState::Offline ||
                             state_ == core::DeviceState::RetryWait;

    if (firstSampleTimedOut || lastSampleStale || driverUnavailable) {
        hasValidData_ = false;
        if (!driverUnavailable) {
            state_ = core::DeviceState::Degraded;
            lastError_ = core::ErrorCode::Timeout;
            condition_ = Scd41Condition::DataNotReadyTimeout;
        }

        if (nextRecoveryAttemptMs_ == 0 ||
            static_cast<int32_t>(nowMs - nextRecoveryAttemptMs_) >= 0) {
            beginRecovery(nowMs);
        }
        return;
    }

    // Latest data is polled every second; the SCD41 normally produces a sample
    // approximately every five seconds.
    bool isDataReady = false;
    uint16_t error = 0;

    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Read, 100);
        if (!lock.acquired()) {
            consecutiveErrors_++;
            lastError_ = core::ErrorCode::Timeout;
            lastRawError_ = 0;
            condition_ = Scd41Condition::LockTimeout;
            services::Logger::warn("SCD41", "SCD41 ts=%u lock=TIMEOUT ready=? read=SKIPPED", nowMs);
            return;
        }
        error = scd4x_.getDataReadyFlag(isDataReady);
    }

    if (error) {
        hal::I2cBus::noteCommunicationError(hal::I2cDevice::Scd41,
                                            hal::I2cOperation::Read);
        readErrorCount_++;
        consecutiveErrors_++;
        lastError_ = core::ErrorCode::BusError;
        lastRawError_ = error;
        condition_ = Scd41Condition::DataReadyError;
        services::Logger::warn("SCD41", "SCD41 ts=%u lock=OK ready=ERR error=%d read=SKIPPED", nowMs, error);
        return;
    }

    if (!isDataReady) {
        notReadyCount_++;
        return;
    }

    uint16_t co2 = 0;
    float temperature = NAN;
    float humidity = NAN;

    {
        hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                               hal::I2cOperation::Measure, 100);
        if (!lock.acquired()) {
            consecutiveErrors_++;
            lastError_ = core::ErrorCode::Timeout;
            lastRawError_ = 0;
            condition_ = Scd41Condition::LockTimeout;
            services::Logger::warn("SCD41", "SCD41 ts=%u lock=TIMEOUT ready=1 read=SKIPPED", nowMs);
            return;
        }
        error = scd4x_.readMeasurement(co2, temperature, humidity);
    }

    if (error || co2 == 0 || !std::isfinite(temperature) || !std::isfinite(humidity)) {
        if (error) {
            hal::I2cBus::noteCommunicationError(hal::I2cDevice::Scd41,
                                                hal::I2cOperation::Measure);
        }
        readErrorCount_++;
        consecutiveErrors_++;
        hasValidData_ = false;
        state_ = core::DeviceState::Degraded;
        lastError_ = core::ErrorCode::ReadFailed;
        lastRawError_ = error;
        condition_ = Scd41Condition::ReadError;
        services::Logger::warn("SCD41", "SCD41 ts=%u lock=OK ready=1 read=NG error=%d co2=%u temp=%.2f rh=%.2f (consec_err=%u)",
            nowMs, error, co2, temperature, humidity, consecutiveErrors_);
        return;
    }

    currentCo2Ppm_ = co2;
    currentTemperature_ = temperature;
    currentHumidity_ = humidity;

    successCount_++;
    consecutiveErrors_ = 0;
    hasValidData_ = true;
    lastSuccessMs_ = nowMs;
    lastError_ = core::ErrorCode::None;
    lastRawError_ = 0;
    state_ = core::DeviceState::Ready;
    condition_ = Scd41Condition::Healthy;
    nextRecoveryAttemptMs_ = 0;

    services::Logger::info("SCD41", "SCD41 ts=%u lock=OK ready=1 read=OK co2=%u temp=%.2f rh=%.2f crc=OK succ=%u err=%u nr=%u",
        nowMs, co2, temperature, humidity, successCount_, readErrorCount_, notReadyCount_);

    if (postFrcLogCount_ > 0) {
        services::Logger::info("SCD41", "event=SCD41_POST_FRC elapsed_ms=%u co2_ppm=%u pressure_hpa=%u",
            nowMs - measurementStartMs_, currentCo2Ppm_, hasAmbientPressure_ ? lastAmbientPressureHpa_ : 0);
        postFrcLogCount_--;
    }
}

Scd41Health Scd41Sensor::health(uint32_t nowMs) const {
    Scd41Health result;
    result.condition = condition_;
    result.state = state_;
    result.lastSuccessMs = lastSuccessMs_;
    result.ageMs = lastSuccessMs_ == 0 ? nowMs - measurementStartMs_ : nowMs - lastSuccessMs_;
    result.rawError = lastRawError_;
    result.consecutiveErrors = consecutiveErrors_;
    return result;
}

bool Scd41Sensor::readEnvironment(core::EnvironmentData& out) const {
    uint32_t nowMs = millis();
    uint32_t ageMs = lastSuccessMs_ == 0 ? UINT32_MAX : nowMs - lastSuccessMs_;

    out.co2Ppm = 0;
    out.co2Valid = false;
    out.co2AgeMs = ageMs;
    out.scd41State = state_;
    out.scd41TemperatureC = NAN;
    out.scd41HumidityRh = NAN;

    if (!hasValidData_ || ageMs > DATA_STALE_MS ||
        state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline ||
        state_ == core::DeviceState::RetryWait) {
        return false;
    }

    out.co2Ppm = currentCo2Ppm_;
    out.co2Valid = true;
    out.scd41TemperatureC = currentTemperature_;
    out.scd41HumidityRh = currentHumidity_;

    if (lastSuccessMs_ > out.timestampMs) out.timestampMs = lastSuccessMs_;
    out.valid = true;
    return true;
}

void Scd41Sensor::setAmbientPressure(uint16_t ambientPressureHpa) {
    if (state_ != core::DeviceState::Ready ||
        recoveryPhase_ != RecoveryPhase::Idle || calibrationInProgress_) {
        return;
    }
    
    hal::I2cLockGuard lock(hal::I2cDevice::Scd41,
                           hal::I2cOperation::Write, 100);
    if (lock.acquired()) {
        uint16_t error = scd4x_.setAmbientPressure(ambientPressureHpa);
        if (error) {
            hal::I2cBus::noteCommunicationError(hal::I2cDevice::Scd41,
                                                hal::I2cOperation::Write);
            services::Logger::warn("SCD41", "Failed to set ambient pressure: %u hPa", ambientPressureHpa);
        } else {
            lastAmbientPressureHpa_ = ambientPressureHpa;
            lastAmbientPressureSetMs_ = millis();
            hasAmbientPressure_ = true;
        }
    }
}

} // namespace sensors
} // namespace drivers
