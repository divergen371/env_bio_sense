#include <Arduino.h>
#include "hal/pins.h"
#include "hal/i2c_bus.h"
#include "services/logger.h"

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

    hal::I2cBus::begin();
    hal::I2cBus::scan();
}

void loop() {
    static uint32_t previousScanMs = 0;

    if (millis() - previousScanMs >= 5000) {
        previousScanMs = millis();

        services::Logger::info("MAIN", "INT levels: MAX30102=%d, BMP585=%d",
            digitalRead(hal::pins::MAX30102_INT),
            digitalRead(hal::pins::BMP585_INT)
        );
    }
}