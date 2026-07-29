#include "services/display_manager.h"
#include "services/logger.h"

namespace services {

constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 64;
constexpr int8_t OLED_RESET = -1; 

DisplayManager::DisplayManager() 
    : display_(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET) {
}

bool DisplayManager::begin() {
    Logger::info("DisplayMgr", "Initializing Display Manager...");
    // SSD1306の初期化 (0x3CはOLEDの一般的なI2Cアドレス)
    if (!display_.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Logger::error("DisplayMgr", "SSD1306 allocation failed or not found at 0x3C");
        available_ = false;
        return false;
    }
    
    available_ = true;
    display_.clearDisplay();
    display_.setTextSize(1);
    display_.setTextColor(SSD1306_WHITE);
    display_.setCursor(0, 0);
    display_.println("Antigravity EnvSense");
    display_.println("Initializing...");
    display_.display();

    Logger::info("DisplayMgr", "OLED Initialized.");
    return true;
}

void DisplayManager::setScreen(ScreenId screen) {
    currentScreen_ = screen;
}

void DisplayManager::render(const core::SensorSnapshot& snapshot, const core::SystemStatus& status, uint32_t nowMs) {
    if (!available_) return;

    display_.clearDisplay();
    
    switch (currentScreen_) {
        case ScreenId::Overview:
            renderOverview(snapshot, status);
            break;
        default:
            display_.setCursor(0, 0);
            display_.println("Screen not impl.");
            break;
    }
    
    display_.display();
}

void DisplayManager::renderOverview(const core::SensorSnapshot& snapshot, const core::SystemStatus& status) {
    display_.setTextSize(1);
    display_.setCursor(0, 0);
    
    if (snapshot.environment.valid) {
        display_.printf("CO2  : %u ppm\n", snapshot.environment.co2Ppm);
        display_.printf("T    : %.1f C\n", snapshot.environment.temperatureC);
        display_.printf("RH   : %.1f %%\n", snapshot.environment.humidityRh);
        display_.printf("P    : %.1f hPa\n", snapshot.environment.pressureHpa);
    } else {
        display_.println("Env Data: --");
        display_.println();
        display_.println();
        display_.println();
    }
    
    display_.printf("HR   : -- bpm\n");
    display_.printf("SpO2 : -- %%\n");
}

} // namespace services
