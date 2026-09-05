#include "services/sensor_manager.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include "hal/clock.h"
#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include <cmath>

namespace services {
namespace {

bool appendCalibrationLogValue(char* buffer, size_t capacity,
                               size_t& length, const char* format, ...) {
    if (buffer == nullptr || format == nullptr || length >= capacity) {
        return false;
    }
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(
        buffer + length, capacity - length, format, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= capacity - length) {
        return false;
    }
    length += static_cast<size_t>(written);
    return true;
}

bool appendOptionalCalibrationFloat(char* buffer, size_t capacity,
                                    size_t& length, float value,
                                    unsigned decimals) {
    if (!std::isfinite(value)) {
        return appendCalibrationLogValue(buffer, capacity, length, ",");
    }
    char format[12] {};
    std::snprintf(format, sizeof(format), "%%.%uf,", decimals);
    return appendCalibrationLogValue(
        buffer, capacity, length, format, value);
}

} // namespace

core::SensorSnapshot SensorManager::snapshot() const {
    portENTER_CRITICAL(&publicationMux_);
    const core::SensorSnapshot copy = publishedSnapshot_;
    portEXIT_CRITICAL(&publicationMux_);
    return copy;
}

core::SystemStatus SensorManager::status() const {
    portENTER_CRITICAL(&publicationMux_);
    const core::SystemStatus copy = publishedStatus_;
    portEXIT_CRITICAL(&publicationMux_);
    return copy;
}

bool SensorManager::copyGnss(core::GnssData& out) const {
    portENTER_CRITICAL(&publicationMux_);
    out = publishedSnapshot_.gnss;
    portEXIT_CRITICAL(&publicationMux_);
    return true;
}

void SensorManager::publishSnapshot() {
    portENTER_CRITICAL(&publicationMux_);
    publishedSnapshot_ = snapshot_;
    publishedStatus_ = status_;
    portEXIT_CRITICAL(&publicationMux_);
}

bool SensorManager::begin(storage::StorageManager& storageManager) {
    storage_ = &storageManager;
    Logger::info("SensorMgr", "Initializing Sensor Manager...");
    
    // GNSSの初期化 (早期に行う)
    lc76g_.begin();

    if (sht45_.begin()) {
        status_.sht45State = sht45_.state();
    } else {
        Logger::error("SensorMgr", "Failed to initialize SHT45");
        status_.sht45State = core::DeviceState::Error;
    }
    
    if (!bmp581_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize BMP581");
    } else {
        float offsetHpa, tempC, slpHpa;
        uint32_t epoch;
        if (storage_->getBmp581Calibration(offsetHpa, epoch, tempC, slpHpa)) {
            bmp581_.setCalibrationOffset(offsetHpa);
            if (std::isfinite(slpHpa) && slpHpa > 800.0f && slpHpa < 1200.0f) {
                setSeaLevelPressure(slpHpa,
                                    core::PressureFieldState::StaticFallback,
                                    core::PressureReferenceSource::Stored,
                                    UINT32_MAX);
            }
            services::Logger::info("SensorMgr", "BMP581 calibration loaded: %.2f hPa (Saved SLP: %.2f hPa)", offsetHpa, slpHpa);
        }
    }

    if (!scd41_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize SCD41");
    }

    if (!sgp41_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize SGP41");
    } else {
        // SGP41の初期化が成功した場合、保存されていたアルゴリズム状態（ベースライン）を復元
        float voc0, voc1;
        if (storage_->getSgp41States(voc0, voc1)) {
            sgp41_.setAlgorithmStates(voc0, voc1);
            services::Logger::info("SensorMgr", "SGP41 VOC algorithm states restored from FRAM.");
        }
    }

    if (!max30102_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize MAX30102");
    }

    if (bme690_.begin()) {
        status_.bme690State = bme690_.state();
    } else {
        services::Logger::error("SensorMgr", "Failed to initialize BME690");
        status_.bme690State = core::DeviceState::Error;
    }

    Logger::info("SensorMgr", "Sensor Manager initialized.");
    
    status_.max30102State = max30102_.state();
    publishSnapshot();
    
    return true;
}

void SensorManager::setSeaLevelPressure(float hpa,
                                        core::PressureFieldState state,
                                        core::PressureReferenceSource source,
                                        uint32_t sourceAgeMs,
                                        uint8_t usedStationCount) {
    if (!std::isfinite(hpa) || hpa <= 800.0f || hpa >= 1200.0f) {
        Logger::error("SensorMgr", "Rejected invalid sea-level pressure: %.2f", hpa);
        return;
    }
    const float previousHpa = bmp581_.getSeaLevelPressure();
    const core::PressureFieldState previousState =
        bmp581_.getPressureFieldState();
    pressureReferenceUpdatedMs_ = sourceAgeMs == UINT32_MAX
        ? 0 : millis() - sourceAgeMs;
    pressureReferenceSource_ = source;
    pressureReferenceStationCount_ = usedStationCount;
    bmp581_.setSeaLevelPressure(hpa, state);

    if (storage_ != nullptr &&
        (!std::isfinite(previousHpa) || std::fabs(previousHpa - hpa) >= 0.01f)) {
        const uint16_t previousDeci = std::isfinite(previousHpa)
            ? static_cast<uint16_t>(std::lround(previousHpa * 10.0f))
            : UINT16_MAX;
        const uint16_t currentDeci =
            static_cast<uint16_t>(std::lround(hpa * 10.0f));
        const uint32_t detail = (static_cast<uint32_t>(previousDeci) << 16u) |
            currentDeci;
        storage_->appendEvent(storage::EventCode::PressureFieldUpdated,
                              static_cast<int32_t>(detail), millis());
    }
    if (previousState != state && storage_ != nullptr) {
        const uint32_t detail = static_cast<uint8_t>(previousState) |
            (static_cast<uint32_t>(static_cast<uint8_t>(state)) << 8u) |
            (static_cast<uint32_t>(static_cast<uint8_t>(source)) << 16u);
        storage_->appendEvent(storage::EventCode::PressureFieldStateChanged,
                              static_cast<int32_t>(detail), millis());
    }
}

void SensorManager::setSeaLevelPressureState(core::PressureFieldState state) {
    const core::PressureFieldState previous = bmp581_.getPressureFieldState();
    if (previous == state) return;
    bmp581_.setPressureFieldState(state);
    if (storage_ != nullptr) {
        const uint32_t detail = static_cast<uint8_t>(previous) |
            (static_cast<uint32_t>(static_cast<uint8_t>(state)) << 8u) |
            (static_cast<uint32_t>(
                static_cast<uint8_t>(pressureReferenceSource_)) << 16u);
        storage_->appendEvent(storage::EventCode::PressureFieldStateChanged,
                              static_cast<int32_t>(detail), millis());
    }
}

bool SensorManager::calibrateScd41(uint16_t referencePpm, drivers::sensors::Scd41FrcResult& result) {
    bool success = scd41_.performForcedRecalibration(referencePpm, result);
    if (storage_ != nullptr) {
        const uint32_t detail =
            (static_cast<uint32_t>(result.rawWord) << 16u) |
            static_cast<uint32_t>(result.referencePpm);
        storage_->appendEvent(
            success ? storage::EventCode::Scd41FrcSucceeded
                    : storage::EventCode::Scd41FrcFailed,
            static_cast<int32_t>(detail), millis());
    }
    if (success) {
        services::Logger::info("SensorMgr", "SCD41 manual calibration succeeded (reference: %u ppm, correction: %d ppm)", referencePpm, result.correctionPpm);
        if (storage_ != nullptr && hal::Clock::isTimeSet()) {
            storage_->setScd41LastCalibrationEpoch(hal::Clock::getEpoch());
        }
    } else {
        services::Logger::error("SensorMgr", "SCD41 manual calibration failed");
    }
    return success;
}

bool SensorManager::factoryResetScd41() {
    const bool success = scd41_.factoryResetAndReconfigure();
    if (storage_ != nullptr) {
        storage_->appendEvent(
            success ? storage::EventCode::Scd41FactoryResetSucceeded
                    : storage::EventCode::Scd41FactoryResetFailed,
            0, millis());
        if (success) storage_->setScd41LastCalibrationEpoch(0);
    }
    return success;
}

bool SensorManager::triggerSht45Heater() {
    bool success = sht45_.triggerHeater();
    if (success) {
        lastHeaterRunMs_ = status_.uptimeMs;
        highHumidityStartMs_ = 0; // Reset monitor
        services::Logger::info("SensorMgr", "SHT45 heater triggered manually or automatically");
    } else {
        services::Logger::error("SensorMgr", "Failed to trigger SHT45 heater");
    }
    return success;
}

bool SensorManager::startBmp581Calibration(float referenceAltitudeM) {
    if (bmp581_.startCalibration(referenceAltitudeM)) {
        bmp581CalibrationStartEpoch_ = hal::Clock::isTimeSet()
            ? hal::Clock::getEpoch() : 0;
        services::Logger::info("SensorMgr", "BMP581 calibration triggered for target %.1f m", referenceAltitudeM);
        return true;
    }
    return false;
}

void SensorManager::trackScd41Health(uint32_t nowMs) {
    if (storage_ == nullptr || !storage_->isFramAvailable()) return;

    drivers::sensors::Scd41Health health = scd41_.health(nowMs);
    bool changed = !scd41ConditionInitialized_ || health.condition != lastScd41Condition_;
    if (!changed) return;

    uint32_t ageSeconds = health.ageMs == UINT32_MAX
        ? 0xFFFFu
        : (health.ageMs / 1000u > 0xFFFFu ? 0xFFFFu : health.ageMs / 1000u);
    int32_t detail = static_cast<int32_t>(
        (static_cast<uint32_t>(health.rawError) << 16) | ageSeconds);

    bool persisted = true;
    bool nextFaultActive = scd41FaultActive_;
    auto record = [&](storage::EventCode code) {
        if (!storage_->appendEvent(code, detail, nowMs)) {
            persisted = false;
            Logger::warn("SensorMgr", "Failed to persist SCD41 event code=0x%02X",
                static_cast<unsigned>(code));
        }
    };

    using drivers::sensors::Scd41Condition;
    switch (health.condition) {
        case Scd41Condition::Healthy:
            if (scd41FaultActive_) record(storage::EventCode::Scd41Recovered);
            nextFaultActive = false;
            break;
        case Scd41Condition::AwaitingFirstSample:
            if (scd41ConditionInitialized_ &&
                lastScd41Condition_ == Scd41Condition::RecoveryWaiting) {
                record(storage::EventCode::Scd41RecoveryRestarted);
            }
            break;
        case Scd41Condition::LockTimeout:
            record(storage::EventCode::Scd41LockTimeout);
            nextFaultActive = true;
            break;
        case Scd41Condition::DataReadyError:
            record(storage::EventCode::Scd41DataReadyError);
            nextFaultActive = true;
            break;
        case Scd41Condition::DataNotReadyTimeout:
            record(storage::EventCode::Scd41Stale);
            nextFaultActive = true;
            break;
        case Scd41Condition::ReadError:
            record(storage::EventCode::Scd41ReadError);
            nextFaultActive = true;
            break;
        case Scd41Condition::DriverError:
            record(storage::EventCode::Scd41DriverError);
            nextFaultActive = true;
            break;
        case Scd41Condition::RecoveryStopping:
            record(storage::EventCode::Scd41RecoveryAttempt);
            nextFaultActive = true;
            break;
        case Scd41Condition::RecoveryWaiting:
            if (!scd41FaultActive_) record(storage::EventCode::Scd41Stale);
            record(storage::EventCode::Scd41RecoveryAttempt);
            nextFaultActive = true;
            break;
        case Scd41Condition::RecoveryFailed:
            record(storage::EventCode::Scd41RecoveryFailed);
            nextFaultActive = true;
            break;
        case Scd41Condition::Stabilizing:
            record(storage::EventCode::Scd41Stabilizing);
            nextFaultActive = true;
            break;
    }

    // If FRAM was temporarily unavailable, keep the previous transition state
    // so the next one-second update retries persisting the event.
    if (!persisted) return;

    scd41FaultActive_ = nextFaultActive;
    lastScd41Condition_ = health.condition;
    scd41ConditionInitialized_ = true;
}

void SensorManager::trackI2cHealth(uint32_t nowMs) {
    if (storage_ == nullptr || !storage_->isFramAvailable()) return;

    constexpr uint32_t EVENT_INTERVAL_MS = 60000u;
    for (uint8_t deviceIndex = 0;
         deviceIndex < static_cast<uint8_t>(hal::I2cDevice::Count);
         ++deviceIndex) {
        for (uint8_t operationIndex = 0;
             operationIndex < static_cast<uint8_t>(hal::I2cOperation::Count);
             ++operationIndex) {
            const auto device = static_cast<hal::I2cDevice>(deviceIndex);
            const auto operation = static_cast<hal::I2cOperation>(operationIndex);
            const hal::I2cDiagnosticCounters current =
                hal::I2cBus::diagnostics(device, operation);
            hal::I2cDiagnosticCounters& persisted =
                persistedI2cCounters_[deviceIndex][operationIndex];

            auto persistDelta = [&](uint32_t currentCount,
                                    uint32_t& persistedCount,
                                    uint32_t& lastEventMs,
                                    storage::EventCode code) {
                if (currentCount <= persistedCount ||
                    (lastEventMs != 0 && nowMs - lastEventMs < EVENT_INTERVAL_MS)) {
                    return;
                }
                const uint32_t rawDelta = currentCount - persistedCount;
                const uint16_t delta = static_cast<uint16_t>(
                    rawDelta > UINT16_MAX ? UINT16_MAX : rawDelta);
                // detail: bits 0..7 device, 8..15 operation, 16..31 delta.
                const uint32_t packed = static_cast<uint32_t>(deviceIndex) |
                    (static_cast<uint32_t>(operationIndex) << 8u) |
                    (static_cast<uint32_t>(delta) << 16u);
                if (storage_->appendEvent(code, static_cast<int32_t>(packed), nowMs)) {
                    persistedCount = currentCount;
                    lastEventMs = nowMs;
                }
            };

            persistDelta(current.lockTimeouts, persisted.lockTimeouts,
                         lastI2cLockEventMs_[deviceIndex][operationIndex],
                         storage::EventCode::I2cLockTimeout);
            persistDelta(current.communicationErrors, persisted.communicationErrors,
                         lastI2cCommunicationEventMs_[deviceIndex][operationIndex],
                         storage::EventCode::I2cCommunicationError);
        }
    }
}

void SensorManager::update(uint32_t nowMs) {
    status_.uptimeMs = nowMs;
    
    static uint32_t lastEnvUpdateMs = 0;
    static uint32_t lastSgp41SaveMs = 0;
    
    // 環境センサ類は 1000ms 間隔で更新
    if (nowMs - lastEnvUpdateMs >= 1000) {
        lastEnvUpdateMs = nowMs;
        
        // SGP41ベースラインの定期的保存 (1時間に1回)
        if (nowMs - lastSgp41SaveMs >= 3600000) {
            lastSgp41SaveMs = nowMs;
            if (sgp41_.state() == core::DeviceState::Ready) {
                float voc0, voc1;
                sgp41_.getAlgorithmStates(voc0, voc1);
                storage_->setSgp41States(voc0, voc1);
                services::Logger::info("SensorMgr", "SGP41 VOC algorithm states saved to FRAM.");
            }
        }
        
        sht45_.update(nowMs);
        status_.sht45State = sht45_.state();
        
        scd41_.update(nowMs);
        status_.scd41State = scd41_.state();
        trackScd41Health(nowMs);
        
        // SGP41 needs temperature and humidity for compensation
        // We prefer SHT45 data as it represents ambient air
        float compTemp = NAN;
        float compHum = NAN;
        if (sht45_.state() == core::DeviceState::Ready) {
            core::EnvironmentData envTmp;
            if (sht45_.readEnvironment(envTmp)) {
                compTemp = envTmp.temperatureC;
                compHum = envTmp.humidityRh;
                
                // --- SHT45 Auto Heater Logic (Condensation Prevention) ---
                if (envTmp.humidityRh >= 95.0f) {
                    if (highHumidityStartMs_ == 0) {
                        highHumidityStartMs_ = nowMs;
                    } else if (nowMs - highHumidityStartMs_ >= 3600000) { // 1 hour continuous >= 95%
                        if (nowMs - lastHeaterRunMs_ >= 3600000) { // Max once per hour
                            services::Logger::info("SensorMgr", "High humidity detected for 1 hour. Triggering heater.");
                            triggerSht45Heater();
                        }
                    }
                } else {
                    highHumidityStartMs_ = 0;
                }
            }
        }
        sgp41_.setCompensation(compTemp, compHum);
        bmp581_.setReferenceTemperature(compTemp);
        sgp41_.update(nowMs);
    }
    
    // BMP581は通常1000ms、校正中は100ms間隔で更新
    static uint32_t lastBmpUpdateMs = 0;
    uint32_t bmpInterval = bmp581_.isCalibrating() ? 100 : 1000;
    if (nowMs - lastBmpUpdateMs >= bmpInterval) {
        lastBmpUpdateMs = nowMs;
        bmp581_.update(nowMs);
        status_.bmp581State = bmp581_.state();
        
        // 校正候補は受入試験とFRAM書込み・読戻しに成功した場合だけ適用する。
        utils::bmp581_calibration::Result calibration;
        if (bmp581_.takeCalibrationResult(calibration)) {
            const bool candidateAccepted = calibration.success;
            const uint32_t calibrationEndEpoch = hal::Clock::isTimeSet()
                ? hal::Clock::getEpoch() : 0;
            bool persisted = false;
            if (calibration.success && storage_ != nullptr &&
                hal::Clock::isTimeSet()) {
                const uint32_t epoch = hal::Clock::getEpoch();
                persisted = storage_->setBmp581Calibration(
                    calibration.pressureOffsetHpa, epoch,
                    calibration.meanTemperatureC,
                    calibration.meanSeaLevelPressureHpa);
                float storedOffset = NAN;
                float storedTemperature = NAN;
                float storedSeaLevelPressure = NAN;
                uint32_t storedEpoch = 0;
                persisted = persisted && storage_->getBmp581Calibration(
                    storedOffset, storedEpoch, storedTemperature,
                    storedSeaLevelPressure) &&
                    std::fabs(storedOffset -
                              calibration.pressureOffsetHpa) < 0.0005f &&
                    storedEpoch == epoch;
            }
            if (calibration.success && persisted) {
                bmp581_.setCalibrationOffset(
                    calibration.pressureOffsetHpa);
                storage_->appendEvent(
                    storage::EventCode::Bmp581CalibrationSucceeded,
                    static_cast<int32_t>(std::lround(
                        calibration.pressureOffsetHpa * 1000.0f)), nowMs);
                Logger::info("SensorMgr",
                    "BMP581 calibration persisted and applied: offset=%.3f hPa temp=%.2f C P0=%.2f hPa",
                    calibration.pressureOffsetHpa,
                    calibration.meanTemperatureC,
                    calibration.meanSeaLevelPressureHpa);
            } else {
                if (calibration.success) {
                    calibration.success = false;
                    calibration.failure =
                        utils::bmp581_calibration::Failure::PersistenceFailed;
                }
                if (storage_ != nullptr) {
                    const int32_t detail =
                        (static_cast<int32_t>(calibration.failure) << 24) |
                        static_cast<int32_t>(
                            calibration.validSamples & 0x00FFFFFFu);
                    storage_->appendEvent(
                        storage::EventCode::Bmp581CalibrationFailed,
                        detail, nowMs);
                }
                Logger::error("SensorMgr",
                    "BMP581 calibration not applied: reason=%u",
                    static_cast<unsigned>(calibration.failure));
            }

            if (storage_ != nullptr) {
                const char* temperatureSource = "MIXED";
                if (calibration.externalTemperatureSamples == 0) {
                    temperatureSource = "BMP581";
                } else if (calibration.externalTemperatureSamples ==
                           calibration.validSamples) {
                    temperatureSource = "SHT45";
                }
                char line[512] {};
                size_t length = 0;
                bool logReady = appendCalibrationLogValue(
                    line, sizeof(line), length,
                    "1,FRAM6_CSV7,");
                if (bmp581CalibrationStartEpoch_ != 0) {
                    logReady = logReady && appendCalibrationLogValue(
                        line, sizeof(line), length, "%lu,",
                        static_cast<unsigned long>(
                            bmp581CalibrationStartEpoch_));
                } else {
                    logReady = logReady && appendCalibrationLogValue(
                        line, sizeof(line), length, ",");
                }
                if (calibrationEndEpoch != 0) {
                    logReady = logReady && appendCalibrationLogValue(
                        line, sizeof(line), length, "%lu,",
                        static_cast<unsigned long>(calibrationEndEpoch));
                } else {
                    logReady = logReady && appendCalibrationLogValue(
                        line, sizeof(line), length, ",");
                }
                logReady = logReady && appendCalibrationLogValue(
                    line, sizeof(line), length,
                    "BMP581,13.6,60,300,%lu,%lu,%lu,%lu,%s,",
                    static_cast<unsigned long>(calibration.totalSamples),
                    static_cast<unsigned long>(calibration.validSamples),
                    static_cast<unsigned long>(calibration.usedSamples),
                    static_cast<unsigned long>(
                        calibration.totalSamples > calibration.usedSamples
                            ? calibration.totalSamples - calibration.usedSamples
                            : 0),
                    temperatureSource);
                logReady = logReady && appendOptionalCalibrationFloat(
                    line, sizeof(line), length,
                    calibration.meanTemperatureC, 2);
                logReady = logReady && appendOptionalCalibrationFloat(
                    line, sizeof(line), length,
                    calibration.meanSeaLevelPressureHpa, 2);
                logReady = logReady && appendOptionalCalibrationFloat(
                    line, sizeof(line), length,
                    calibration.meanRawPressureHpa, 3);
                logReady = logReady && appendOptionalCalibrationFloat(
                    line, sizeof(line), length,
                    calibration.meanExpectedPressureHpa, 3);
                logReady = logReady && appendOptionalCalibrationFloat(
                    line, sizeof(line), length,
                    calibration.pressureOffsetHpa, 3);
                logReady = logReady && appendOptionalCalibrationFloat(
                    line, sizeof(line), length,
                    calibration.preCalibrationMeanAltitudeM, 3);
                logReady = logReady && appendOptionalCalibrationFloat(
                    line, sizeof(line), length,
                    calibration.preCalibrationStdDevM, 3);
                logReady = logReady && appendOptionalCalibrationFloat(
                    line, sizeof(line), length,
                    calibration.postCalibrationMeanAltitudeM, 3);
                if (std::isfinite(calibration.postCalibrationStdDevM)) {
                    logReady = logReady && appendCalibrationLogValue(
                        line, sizeof(line), length, "%.3f,",
                        calibration.postCalibrationStdDevM);
                } else {
                    logReady = logReady && appendCalibrationLogValue(
                        line, sizeof(line), length, ",");
                }
                logReady = logReady && appendCalibrationLogValue(
                    line, sizeof(line), length, "%u,%u,%s",
                    candidateAccepted ? 1u : 0u,
                    persisted ? 1u : 0u,
                    utils::bmp581_calibration::failureName(
                        calibration.failure));
                if (!logReady ||
                    !storage_->appendBmp581CalibrationLogLine(line)) {
                    Logger::error("SensorMgr",
                        "Failed to persist BMP581 calibration detail log");
                }
            }
            bmp581CalibrationStartEpoch_ = 0;
        }
        
        // BMP581の気圧データをSCD41の補償に渡す
        if (bmp581_.state() == core::DeviceState::Ready) {
            core::EnvironmentData envTmp;
            if (bmp581_.readEnvironment(envTmp) && std::isfinite(envTmp.pressureHpa)) {
                uint16_t pressInt = static_cast<uint16_t>(envTmp.pressureHpa);
                if (pressInt >= 700 && pressInt <= 1200) {
                    scd41_.setAmbientPressure(pressInt);
                }
            }
        }
    }
        

    
    // MAX30102 (脈波センサ) は FIFO の取りこぼしを防ぐため常に更新する
    max30102_.update(nowMs);
    status_.max30102State = max30102_.state();

    // BME690 (非ブロッキング状態機械のため頻繁に呼ぶ)
    bme690_.update(nowMs);
    status_.bme690State = bme690_.state();
    
    // スナップショットに反映。SCD41が読めなかった周期に前回値を
    // 持ち越さないよう、CO2固有フィールドは毎回明示的に初期化する。
    snapshot_.environment.co2Ppm = 0;
    snapshot_.environment.co2Valid = false;
    snapshot_.environment.co2AgeMs = UINT32_MAX;
    snapshot_.environment.scd41State = scd41_.state();
    snapshot_.environment.scd41TemperatureC = NAN;
    snapshot_.environment.scd41HumidityRh = NAN;
    snapshot_.environment.temperatureC = NAN;
    snapshot_.environment.humidityRh = NAN;
    snapshot_.environment.temperatureValid = false;
    snapshot_.environment.humidityValid = false;
    snapshot_.environment.pressureValid = false;
    snapshot_.environment.pressureStale = false;
    snapshot_.environment.altitudeValid = false;
    snapshot_.environment.sgp41Valid = false;

    bool envValid = false;
    if (sht45_.readEnvironment(snapshot_.environment)) {
        envValid = true;
    }
    if (bmp581_.readEnvironment(snapshot_.environment)) {
        envValid = true;
    }
    if (scd41_.readEnvironment(snapshot_.environment)) {
        envValid = true;
    }
    if (sgp41_.readEnvironment(snapshot_.environment)) {
        envValid = true;
    }
    
    snapshot_.environment.valid = envValid;

    // BME690 データ取得
    bme690_.readData(snapshot_.bme690);

    // --- Enclosure Warning (空気循環・熱ごもり異常検知) ---
    if (envValid && sht45_.state() == core::DeviceState::Ready && scd41_.state() == core::DeviceState::Ready) {
        // SCD41(内部)とSHT45(外気)の温度差が5.0℃以上なら熱ごもりと判定
        float tempDiff = snapshot_.environment.scd41TemperatureC - snapshot_.environment.temperatureC;
        if (tempDiff >= 5.0f) {
            snapshot_.environment.enclosureWarning = core::EnclosureWarning::HeatTrapped;
        } else {
            snapshot_.environment.enclosureWarning = core::EnclosureWarning::Normal;
        }
    }

    // --- PPG Data Aggregation ---
    if (max30102_.readPpg(snapshot_.ppg)) {
        // Step 6 では生データのみが反映される
    }

    // GNSSは常に細かくupdateを呼んでUARTバッファからNMEAをパースする
    lc76g_.update(nowMs);
    lc76g_.readGnss(snapshot_.gnss, nowMs);
    status_.gnss = lc76g_.status(nowMs);

    // NMEAから得られた最新のUTCと、それに紐づくPPSタイムスタンプで時刻同期を試みる
    gnssTimeSync_.update(snapshot_.gnss, nowMs);

    // PPSイベントフラグをクリアしておく
    drivers::sensors::PpsEvent pps;
    while (lc76g_.takePpsEvent(pps)) {}
    
    // NMEAが途絶えたらHoldoverへ移行するなどの処理が必要
    if (!status_.gnss.nmeaAlive || !status_.gnss.ppsRecent) {
        gnssTimeSync_.reportHoldover(nowMs);
    }
    
    // 現在のタイムソースをSystemStatusへ反映
    status_.timeSource = hal::Clock::source();
    
    // TimeDisciplinedフラグを反映
    status_.gnss.timeDisciplined = hal::Clock::isDisciplined();

    auto health = [nowMs](const drivers::sensors::ISensor& sensor,
                          uint32_t consecutiveErrors) {
        core::SensorHealthSnapshot result;
        result.state = sensor.state();
        result.error = sensor.lastError();
        result.consecutiveErrors = consecutiveErrors;
        result.ageMs = sensor.lastSuccessMs() == 0
            ? UINT32_MAX : nowMs - sensor.lastSuccessMs();
        return result;
    };
    snapshot_.telemetry.sht45 = health(sht45_, sht45_.consecutiveErrors());
    snapshot_.telemetry.bmp581 = health(bmp581_, bmp581_.consecutiveErrors());
    const drivers::sensors::Scd41Health scdHealth = scd41_.health(nowMs);
    snapshot_.telemetry.scd41.state = scdHealth.state;
    snapshot_.telemetry.scd41.error = scd41_.lastError();
    snapshot_.telemetry.scd41.ageMs = scdHealth.ageMs;
    snapshot_.telemetry.scd41.consecutiveErrors = scdHealth.consecutiveErrors;
    snapshot_.telemetry.scd41RawError = scdHealth.rawError;
    snapshot_.telemetry.sgp41 = health(sgp41_, sgp41_.consecutiveErrors());
    snapshot_.telemetry.bme690 = health(bme690_, bme690_.consecutiveErrors());
    sgp41_.getRawTelemetry(
        snapshot_.telemetry.sgp41Raw.srawVoc,
        snapshot_.telemetry.sgp41Raw.srawNox,
        snapshot_.telemetry.sgp41Raw.compensationRhTicks,
        snapshot_.telemetry.sgp41Raw.compensationTemperatureTicks);
    snapshot_.telemetry.altitude.rawAltitudeM = bmp581_.getRawAltitude();
    snapshot_.telemetry.altitude.displayAltitudeM = bmp581_.getDisplayAltitude();
    snapshot_.telemetry.altitude.seaLevelPressureHpa = bmp581_.getSeaLevelPressure();
    snapshot_.telemetry.altitude.pressureOffsetHpa = bmp581_.getCalibrationOffset();
    bool externalAltitudeTemperature = false;
    bmp581_.getAltitudeTemperature(
        snapshot_.telemetry.altitude.calculationTemperatureC,
        externalAltitudeTemperature);
    snapshot_.telemetry.altitude.externalTemperatureSource =
        externalAltitudeTemperature;
    snapshot_.telemetry.altitude.usedStationCount =
        pressureReferenceStationCount_;
    snapshot_.telemetry.altitude.pressureState = bmp581_.getPressureFieldState();
    snapshot_.telemetry.altitude.pressureSource = pressureReferenceSource_;
    snapshot_.telemetry.altitude.seaLevelPressureAgeMs =
        pressureReferenceUpdatedMs_ == 0
            ? UINT32_MAX : nowMs - pressureReferenceUpdatedMs_;
    const hal::I2cDiagnosticCounters i2c = hal::I2cBus::diagnosticTotals();
    snapshot_.telemetry.i2c.lockTimeouts = i2c.lockTimeouts;
    snapshot_.telemetry.i2c.communicationErrors = i2c.communicationErrors;
    trackI2cHealth(nowMs);
    publishSnapshot();
}

} // namespace services
