#include "services/sensor_manager.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include "hal/clock.h"
#include <Arduino.h>
#include <cmath>

namespace services {

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
                setSeaLevelPressure(slpHpa, core::PressureFieldState::LastKnown,
                                    core::PressureReferenceSource::Stored);
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
                                        core::PressureReferenceSource source) {
    if (state == core::PressureFieldState::Valid || state == core::PressureFieldState::LastKnown) {
        pressureReferenceUpdatedMs_ = millis();
        pressureReferenceSource_ = source;
        if (source == core::PressureReferenceSource::Amedas ||
            source == core::PressureReferenceSource::Manual) {
            lastAmedasUpdateMs_ = pressureReferenceUpdatedMs_;
            slpEma_ = hpa;
        }
    } else {
        pressureReferenceSource_ = core::PressureReferenceSource::Unset;
    }
    bmp581_.setSeaLevelPressure(hpa, state);
}

bool SensorManager::calibrateScd41(uint16_t referencePpm, drivers::sensors::Scd41FrcResult& result) {
    bool success = scd41_.performForcedRecalibration(referencePpm, result);
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
    return scd41_.factoryResetAndReconfigure();
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
    }

    // If FRAM was temporarily unavailable, keep the previous transition state
    // so the next one-second update retries persisting the event.
    if (!persisted) return;

    scd41FaultActive_ = nextFaultActive;
    lastScd41Condition_ = health.condition;
    scd41ConditionInitialized_ = true;
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
        
        // 校正完了のチェックと永続化
        if (!bmp581_.isCalibrating() && bmpInterval == 100) {
            float offset = bmp581_.getCalibrationOffset();
            if (offset != 0.0f) { // If offset changed, we can assume success for now, or just force save
                float slp = bmp581_.getSeaLevelPressure();
                storage_->setBmp581Calibration(offset, hal::Clock::getEpoch(), NAN, slp); 
            }
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

    // --- GNSS based SLP calibration ---
    static uint32_t lastGnssSlpUpdateMs = 0;
    if (nowMs - lastGnssSlpUpdateMs >= 1000) { // Update EMA once per second
        lastGnssSlpUpdateMs = nowMs;
        
        // 2 hours without AMeDAS, or AMeDAS never fetched
        if (nowMs - lastAmedasUpdateMs_ >= 7200000 || lastAmedasUpdateMs_ == 0) {
            if (snapshot_.gnss.fixValid && snapshot_.gnss.hdop < 2.0f && snapshot_.gnss.speedMps < 0.5f) {
                if (snapshot_.environment.pressureValid && !std::isnan(snapshot_.environment.pressureHpa)) {
                    float currentP = snapshot_.environment.pressureHpa;
                    float currentT = std::isnan(snapshot_.environment.temperatureC) ? 20.0f : snapshot_.environment.temperatureC;
                    if (!std::isnan(snapshot_.gnss.altitudeMslM)) {
                        float tempK = currentT + 273.15f;
                        float expVal = 1.0f - (0.0065f * snapshot_.gnss.altitudeMslM) / tempK;
                        if (expVal > 0.0f) {
                            float impliedSlp = currentP / std::pow(expVal, 5.254999f);
                            
                            if (std::isnan(slpEma_)) {
                                slpEma_ = impliedSlp;
                            } else {
                                // alpha = 1 / 3600 (approx 1 hour time constant at 1Hz)
                                slpEma_ = slpEma_ + (impliedSlp - slpEma_) / 3600.0f;
                            }
                            
                            // Apply to BMP581
                            setSeaLevelPressure(slpEma_, core::PressureFieldState::Valid,
                                                core::PressureReferenceSource::Gnss);
                        }
                    }
                }
            }
        }
    }

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
    snapshot_.telemetry.altitude.pressureState = bmp581_.getPressureFieldState();
    snapshot_.telemetry.altitude.pressureSource = pressureReferenceSource_;
    snapshot_.telemetry.altitude.seaLevelPressureAgeMs =
        pressureReferenceUpdatedMs_ == 0
            ? UINT32_MAX : nowMs - pressureReferenceUpdatedMs_;
    const hal::I2cDiagnosticCounters i2c = hal::I2cBus::diagnosticTotals();
    snapshot_.telemetry.i2c.lockTimeouts = i2c.lockTimeouts;
    snapshot_.telemetry.i2c.communicationErrors = i2c.communicationErrors;
    publishSnapshot();
}

} // namespace services
