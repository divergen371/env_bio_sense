#include <Arduino.h>
#include <Wire.h>

namespace Pins {
constexpr uint8_t I2C_SDA = D4;       // GPIO6
constexpr uint8_t I2C_SCL = D5;       // GPIO7
constexpr uint8_t MAX30102_INT = D2;  // GPIO2
constexpr uint8_t BMP585_INT = D3;    // GPIO3
}

void scanI2cBus() {
    Serial.println();
    Serial.println("I2C scan started.");

    uint8_t deviceCount = 0;

    for (uint8_t address = 1; address < 127; ++address) {
        Wire.beginTransmission(address);
        const uint8_t error = Wire.endTransmission();

        if (error == 0) {
            Serial.printf("Found device at 0x%02X\n", address);
            ++deviceCount;
        } else if (error == 4) {
            Serial.printf("Unknown error at 0x%02X\n", address);
        }
    }

    if (deviceCount == 0) {
        Serial.println("No I2C devices found.");
    } else {
        Serial.printf("Scan complete: %u device(s) found.\n", deviceCount);
    }
}

void setup() {
    Serial.begin(115200);

    // USB CDC接続を少し待つ。永久待機にはしない。
    const uint32_t serialWaitStarted = millis();
    while (!Serial && millis() - serialWaitStarted < 3000) {
        delay(10);
    }

    Serial.println();
    Serial.println("XIAO ESP32S3 sensor station");
    Serial.println("----------------------------");

    pinMode(Pins::MAX30102_INT, INPUT_PULLUP);
    pinMode(Pins::BMP585_INT, INPUT_PULLUP);

    Wire.begin(Pins::I2C_SDA, Pins::I2C_SCL);
    Wire.setClock(100000);

    Serial.printf("SDA: D4 / GPIO%u\n", Pins::I2C_SDA);
    Serial.printf("SCL: D5 / GPIO%u\n", Pins::I2C_SCL);
    Serial.printf("MAX30102 INT: D2 / GPIO%u\n", Pins::MAX30102_INT);
    Serial.printf("BMP585 INT: D3 / GPIO%u\n", Pins::BMP585_INT);

    scanI2cBus();
}

void loop() {
    static uint32_t previousScanMs = 0;

    if (millis() - previousScanMs >= 5000) {
        previousScanMs = millis();

        Serial.printf(
            "INT levels: MAX30102=%d, BMP585=%d\n",
            digitalRead(Pins::MAX30102_INT),
            digitalRead(Pins::BMP585_INT)
        );
    }
}