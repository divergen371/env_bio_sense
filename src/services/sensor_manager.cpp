#include "services/sensor_manager.h"
#include "services/logger.h"

namespace services {

bool SensorManager::begin() {
    Logger::info("SensorMgr", "Initializing Sensor Manager...");
    
    if (sht45_.begin()) {
        status_.sht45State = sht45_.state();
    } else {
        Logger::error("SensorMgr", "Failed to initialize SHT45");
        status_.sht45State = core::DeviceState::Error;
    }
    
    if (!bmp585_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize BMP585");
    }

    if (!scd41_.begin()) {
        services::Logger::error("SensorMgr", "Failed to initialize SCD41");
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
        
        bmp585_.update(nowMs);
        status_.bmp585State = bmp585_.state();
        
        scd41_.update(nowMs);
        status_.scd41State = scd41_.state();
    }
    
    // MAX30102 (脈波センサ) は FIFO の取りこぼしを防ぐため常に更新する
    max30102_.update(nowMs);
    status_.max30102State = max30102_.state();
    
    // スナップショットに反映
    bool envValid = false;
    if (sht45_.readEnvironment(snapshot_.environment)) {
        envValid = true;
    }
    if (bmp585_.readEnvironment(snapshot_.environment)) {
        envValid = true;
    }
    if (scd41_.readEnvironment(snapshot_.environment)) {
        envValid = true;
    }
    
    snapshot_.environment.valid = envValid;

    // --- PPG Data Aggregation ---
    if (max30102_.readPpg(snapshot_.ppg)) {
        // Step 6 では生データのみが反映される
    }
}

} // namespace services
