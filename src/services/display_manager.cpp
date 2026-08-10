#include "services/display_manager.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"

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
    bool initOk = false;
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) {
            initOk = display_.begin(SSD1306_SWITCHCAPVCC, 0x3C);
        } else {
            Logger::error("DisplayMgr", "Failed to acquire lock for OLED begin");
        }
    }

    if (!initOk) {
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
    
    {
        hal::I2cLockGuard lock(100);
        if (lock.acquired()) display_.display();
    }

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
    
    hal::I2cLockGuard lock(100);
    if (lock.acquired()) {
        display_.display();
    } else {
        services::Logger::warn("DisplayMgr", "Failed to acquire lock for OLED display");
    }
}

void DisplayManager::renderOverview(const core::SensorSnapshot& snapshot, const core::SystemStatus& status) {
    display_.setTextSize(1);
    display_.setCursor(0, 0);
    
    if (snapshot.environment.valid) {
        display_.printf("CO2  : %u ppm\n", snapshot.environment.co2Ppm);
        display_.printf("T    : %.1f C\n", snapshot.environment.temperatureC);
        display_.printf("RH   : %.1f %%\n", snapshot.environment.humidityRh);
        
        if (!snapshot.environment.pressureValid) {
            display_.println("P    : SENSOR ERROR");
        } else if (snapshot.environment.pressureStale) {
            display_.printf("P    : %.1f [STALE]\n", snapshot.environment.pressureHpa);
        } else {
            display_.printf("P    : %.1f hPa\n", snapshot.environment.pressureHpa);
        }
        
        if (snapshot.environment.sgp41Valid) {
            display_.printf("VOC  : %d   NOx: %d\n", snapshot.environment.vocIndex, snapshot.environment.noxIndex);
        } else {
            display_.println("VOC  : --    NOx: --");
        }
    } else {
        display_.println("Env Data: --");
        display_.println();
        display_.println();
        display_.println();
        display_.println();
    }
    
    if (status.max30102State == core::DeviceState::Error || status.max30102State == core::DeviceState::Offline) {
        display_.printf("HR   : Error\n");
        display_.printf("SpO2 : Error\n");
    } else if (snapshot.ppg.state == core::PpgState::NoFinger) {
        display_.printf("HR   : No Finger\n");
        display_.printf("SpO2 : -- %%\n");
    } else if (snapshot.ppg.state == core::PpgState::Calibrating) {
        display_.printf("HR   : Calibrating\n");
        display_.printf("SpO2 : -- %%\n");
    } else if (snapshot.ppg.state == core::PpgState::Measuring) {
        if (snapshot.ppg.signalPoor) {
            display_.printf("HR   : Weak Sig\n");
            display_.printf("SpO2 : Adjust Prs\n");
        } else if (snapshot.ppg.calculatedValid) {
            display_.printf("HR   : %.1f bpm\n", snapshot.ppg.heartRateBpm);
            display_.printf("SpO2 : %.1f %%\n", snapshot.ppg.spo2Percent);
        } else {
            display_.printf("HR   : calc...\n");
            display_.printf("SpO2 : calc...\n");
        }
    } else {
        display_.printf("HR   : -- bpm\n");
        display_.printf("SpO2 : -- %%\n");
    }
}

} // namespace services
