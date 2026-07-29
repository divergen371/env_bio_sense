#pragma once

#include "core/sensor_snapshot.h"
#include <cstdint>
#include <Adafruit_SSD1306.h>

namespace services {

enum class ScreenId : uint8_t {
    Overview,
    Environment,
    Ppg,
    Diagnostics
};

class DisplayManager {
public:
    DisplayManager();
    bool begin();
    void setScreen(ScreenId screen);
    void render(const core::SensorSnapshot& snapshot, const core::SystemStatus& status, uint32_t nowMs);
    bool isAvailable() const { return available_; }

private:
    Adafruit_SSD1306 display_;
    ScreenId currentScreen_ {ScreenId::Overview};
    bool available_ {false};

    void renderOverview(const core::SensorSnapshot& snapshot, const core::SystemStatus& status);
};

} // namespace services
