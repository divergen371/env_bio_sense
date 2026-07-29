#include <Arduino.h>
#include "hal/pins.h"
#include "hal/i2c_bus.h"
#include "services/logger.h"
#include "services/sensor_manager.h"
#include "services/display_manager.h"

services::SensorManager sensorManager;
services::DisplayManager displayManager;

void setup() {
    services::Logger::init(115200);

    // USB CDC接続を少し待つ。永久待機にはしない。
    const uint32_t serialWaitStarted = millis();
    while (!Serial && millis() - serialWaitStarted < 3000) {
        delay(10);
    }

    services::Logger::info("MAIN", "");
    services::Logger::info("MAIN", "XIAO ESP32S3 sensor station");
    services::Logger::info("MAIN", "----------------------------");

    pinMode(hal::pins::MAX30102_INT, INPUT_PULLUP);
    pinMode(hal::pins::BMP585_INT, INPUT_PULLUP);

    services::Logger::info("MAIN", "SDA: D4 / GPIO%u", hal::pins::I2C_SDA);
    services::Logger::info("MAIN", "SCL: D5 / GPIO%u", hal::pins::I2C_SCL);
    services::Logger::info("MAIN", "MAX30102 INT: D2 / GPIO%u", hal::pins::MAX30102_INT);
    services::Logger::info("MAIN", "BMP585 INT: D3 / GPIO%u", hal::pins::BMP585_INT);

    // I2Cバス初期化とスキャン
    hal::I2cBus::begin();
    hal::I2cBus::scan();

    // サービス初期化
    sensorManager.begin();
    displayManager.begin();
}

void loop() {
    static uint32_t previousDisplayMs = 0;
    static uint32_t previousSensorMs = 0;
    uint32_t nowMs = millis();

    // センサ更新 (内部で環境系は1000ms間隔、PPG系は常時更新に制御)
    sensorManager.update(nowMs);

    // 500msごとにOLEDを更新
    if (nowMs - previousDisplayMs >= 500) {
        previousDisplayMs = nowMs;
        displayManager.render(sensorManager.snapshot(), sensorManager.status(), nowMs);
    }

    static uint32_t previousLogMs = 0;
    // 2000msごとにPPGの計算結果(HR/SpO2)または生データをシリアルに出力
    if (nowMs - previousLogMs >= 2000) {
        previousLogMs = nowMs;
        const auto& snap = sensorManager.snapshot();
        if (snap.ppg.state == core::PpgState::NoFinger) {
            services::Logger::info("MAIN", "PPG: No Finger");
        } else if (snap.ppg.state == core::PpgState::Calibrating) {
            services::Logger::info("MAIN", "PPG: Calibrating (IR: %u)", snap.ppg.ir);
        } else if (snap.ppg.state == core::PpgState::Measuring) {
            if (snap.ppg.calculatedValid) {
                services::Logger::info("MAIN", "HR: %.1f bpm, SpO2: %.1f %%", snap.ppg.heartRateBpm, snap.ppg.spo2Percent);
            } else {
                services::Logger::info("MAIN", "PPG: Measuring (Calc...)");
            }
        }
    }
}