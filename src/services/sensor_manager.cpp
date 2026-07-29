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
    
    status_.bmp585State = core::DeviceState::Unknown;
    status_.scd41State = core::DeviceState::Unknown;
    status_.max30102State = core::DeviceState::Unknown;
    
    return true;
}

void SensorManager::update(uint32_t nowMs) {
    status_.uptimeMs = nowMs;
    
    // センサ更新
    sht45_.update(nowMs);
    status_.sht45State = sht45_.state();
    
    // スナップショットに反映
    if (sht45_.readEnvironment(snapshot_.environment)) {
        // sht45_.readEnvironment が内部で out.valid = true 等を行う
    } else {
        snapshot_.environment.valid = false;
    }
}

} // namespace services
