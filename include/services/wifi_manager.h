#pragma once
#include <WiFi.h>

namespace services {

class WifiManager {
public:
    WifiManager();
    void begin();
    
    void toggle();
    void turnOn();
    void turnOff();
    
    void update(); // loop内で呼ぶ
    
    bool isOn() const { return isOn_; }

private:
    bool isOn_;
    void startAP();
    void stopAP();
};

} // namespace services
