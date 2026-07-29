#include "services/sensor_manager.h"
#include "services/logger.h"

namespace services {

bool SensorManager::begin() {
    Logger::info("SensorMgr", "Initializing Sensor Manager...");
    // 本来はここで各センサのbegin()を呼ぶ
    // 今回はモックとして状態を準備中(Initializing)からReadyにする
    status_.sht45State = core::DeviceState::Ready;
    status_.bmp585State = core::DeviceState::Ready;
    status_.scd41State = core::DeviceState::Ready;
    status_.max30102State = core::DeviceState::Ready;
    
    return true;
}

void SensorManager::update(uint32_t nowMs) {
    status_.uptimeMs = nowMs;
    // 今回はモックデータを入れる（OLED表示確認用）
    snapshot_.environment.temperatureC = 25.5f;
    snapshot_.environment.humidityRh = 45.2f;
    snapshot_.environment.pressureHpa = 1013.2f;
    snapshot_.environment.co2Ppm = 400;
    snapshot_.environment.valid = true;
    snapshot_.environment.timestampMs = nowMs;
}

} // namespace services
