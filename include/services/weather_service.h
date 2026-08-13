#pragma once

#include <cstdint>
#include <WString.h>
#include "services/sensor_manager.h"
#include "services/wifi_manager.h"

namespace services {

class WeatherService {
public:
    WeatherService(SensorManager& sensorManager, WifiManager& wifiManager);
    
    void begin();
    void update(uint32_t nowMs);
    void forceUpdate(float pressureHpa); // スマホ等からPOSTされた場合用

private:
    SensorManager& sensorManager_;
    WifiManager& wifiManager_;
    
    uint32_t lastFetchMs_ = 0;
    bool needsFetch_ = true;
    
    bool fetchSeaLevelPressure();
};

} // namespace services
