#include "services/sensor_manager.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"

namespace services {

bool SensorManager::begin() {
    Logger::info("SensorMgr", "Initializing Sensor Manager...");
    
    if (sht45_.begin()) {
        status_.sht45State = sht45_.state();
    } else {
        Logger::error("SensorMgr", "Failed to initialize SHT45");
        status_.sht45State = core::DeviceState::Error;
    }
    
    if (!bmp581_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize BMP581");
    }

    if (!scd41_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize SCD41");
    }

    if (!sgp41_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize SGP41");
    }

    if (!max30102_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize MAX30102");
    }

    Logger::info("SensorMgr", "Sensor Manager initialized.");
    
    status_.max30102State = max30102_.state();
    
    return true;
}

void SensorManager::update(uint32_t nowMs) {
    status_.uptimeMs = nowMs;
    
    static uint32_t lastEnvUpdateMs = 0;
    
    // 環境センサ類は 1000ms 間隔で更新
    if (nowMs - lastEnvUpdateMs >= 1000) {
        lastEnvUpdateMs = nowMs;
        
        sht45_.update(nowMs);
        status_.sht45State = sht45_.state();
        
        bmp581_.update(nowMs);
        status_.bmp581State = bmp581_.state();
        
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
            }
        }
        sgp41_.setCompensation(compTemp, compHum);
        sgp41_.update(nowMs);
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
}

} // namespace services
