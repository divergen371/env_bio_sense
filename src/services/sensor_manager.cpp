#include "services/sensor_manager.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include "hal/clock.h"

namespace services {

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
                bmp581_.setSeaLevelPressure(slpHpa, core::PressureFieldState::LastKnown);
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

    Logger::info("SensorMgr", "Sensor Manager initialized.");
    
    status_.max30102State = max30102_.state();
    
    return true;
}

void SensorManager::setSeaLevelPressure(float hpa, core::PressureFieldState state) {
    bmp581_.setSeaLevelPressure(hpa, state);
}

bool SensorManager::calibrateScd41(uint16_t targetPpm, uint16_t& frcCorrection) {
    bool success = scd41_.performManualCalibration(targetPpm, frcCorrection);
    if (success) {
        services::Logger::info("SensorMgr", "SCD41 manual calibration succeeded (target: %u ppm, correction: 0x%04X)", targetPpm, frcCorrection);
        if (storage_ != nullptr && hal::Clock::isTimeSet()) {
            storage_->setScd41LastCalibrationEpoch(hal::Clock::getEpoch());
        }
    } else {
        services::Logger::error("SensorMgr", "SCD41 manual calibration failed");
    }
    return success;
}

bool SensorManager::isScd41CalibrationRecommended() const {
    if (storage_ == nullptr || !hal::Clock::isTimeSet()) {
        return false;
    }
    uint32_t lastEpoch = storage_->getScd41LastCalibrationEpoch();
    if (lastEpoch == 0) return true; // 未校正
    
    uint32_t nowEpoch = hal::Clock::getEpoch();
    if (nowEpoch > lastEpoch) {
        uint32_t diff = nowEpoch - lastEpoch;
        // 7 days = 7 * 24 * 60 * 60 = 604800 seconds
        if (diff >= 604800) {
            return true;
        }
    }
    return false;
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
    
    // スナップショットに反映
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
}

} // namespace services
