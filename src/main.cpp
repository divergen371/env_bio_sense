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

    // 1000msごとにセンサ値を更新 (現在はモック)
    if (nowMs - previousSensorMs >= 1000) {
        previousSensorMs = nowMs;
        sensorManager.update(nowMs);
    }

    // 500msごとにOLEDを更新
    if (nowMs - previousDisplayMs >= 500) {
        previousDisplayMs = nowMs;
        displayManager.render(sensorManager.snapshot(), sensorManager.status(), nowMs);
    }

    static uint32_t previousLogMs = 0;
    // 5000msごとにINTピン状態をログ出力
    if (nowMs - previousLogMs >= 5000) {
        previousLogMs = nowMs;
        services::Logger::info("MAIN", "INT levels: MAX30102=%d, BMP585=%d",
            digitalRead(hal::pins::MAX30102_INT),
            digitalRead(hal::pins::BMP585_INT)
        );
    }
}