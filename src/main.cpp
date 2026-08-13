#include <Arduino.h>
#include "hal/pins.h"
#include "hal/i2c_bus.h"
#include "services/logger.h"
#include "services/sensor_manager.h"
#include "services/display_manager.h"
#include "storage/storage_manager.h"
#include "services/time_manager.h"
#include "services/wifi_manager.h"
#include "services/web_server_service.h"
#include "services/weather_service.h"

services::SensorManager sensorManager;
services::DisplayManager displayManager;
storage::StorageManager storageManager;
services::WifiManager wifiManager;
services::WebServerService webServer(storageManager);
services::WeatherService weatherService(sensorManager, wifiManager);

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
    pinMode(hal::pins::BMP5_INT, INPUT_PULLUP);
    pinMode(hal::pins::USER_LED, OUTPUT);
    pinMode(hal::pins::BOOT_BUTTON, INPUT_PULLUP); // Boot Button for Wi-Fi toggle
    digitalWrite(hal::pins::USER_LED, HIGH); // 消灯 (XIAOのLEDは通常Active-Low)

    services::Logger::info("MAIN", "SDA: D4 / GPIO%u", hal::pins::I2C_SDA);
    services::Logger::info("MAIN", "SCL: D5 / GPIO%u", hal::pins::I2C_SCL);
    services::Logger::info("MAIN", "MAX30102 INT: D2 / GPIO%u", hal::pins::MAX30102_INT);
    services::Logger::info("MAIN", "BMP5 INT: D3 / GPIO%u", hal::pins::BMP5_INT);
    services::Logger::info("MAIN", "--------------------------------");

    // NTPサーバーから時刻を取得 (完了後にWi-Fi切断)
    services::TimeManager::begin();

    // I2Cバス初期化とスキャン
    hal::I2cBus::begin();
    hal::I2cBus::scan();

    // サービス初期化
    sensorManager.begin();
    displayManager.begin();
    storageManager.begin();
    
    wifiManager.begin();
    webServer.begin();
    weatherService.begin();

    // SDカードが使用できない場合はLEDを点灯（XIAOはLOWで点灯）
    if (!storageManager.isSdAvailable()) {
        digitalWrite(hal::pins::USER_LED, LOW);
    }
}

void loop() {
    static uint32_t previousDisplayMs = 0;
    static uint32_t previousSensorMs = 0;
    uint32_t nowMs = millis();
    
    // BOOTボタンによるWi-Fiトグル (3秒長押し判定)
    static uint32_t btnPressedMs = 0;
    static bool btnHandled = false;
    if (digitalRead(hal::pins::BOOT_BUTTON) == LOW) {
        if (btnPressedMs == 0) {
            btnPressedMs = nowMs;
            btnHandled = false;
        } else if (!btnHandled && (nowMs - btnPressedMs >= 3000)) {
            wifiManager.toggle();
            btnHandled = true;
        }
    } else {
        btnPressedMs = 0;
    }
    
    // Wi-Fiモード中はLEDを点滅 (SDエラーがなければ)
    if (storageManager.isSdAvailable()) {
        if (wifiManager.isOn()) {
            digitalWrite(hal::pins::USER_LED, (nowMs % 1000 < 500) ? LOW : HIGH);
        } else {
            digitalWrite(hal::pins::USER_LED, HIGH);
        }
    }
    
    
    // Wi-Fiモード中はSDへの新規書き込みを停止させる
    storageManager.setWifiActive(wifiManager.isOn());

    // センサ更新 (内部で環境系は1000ms間隔、PPG系は常時更新に制御)
    sensorManager.update(nowMs);
    
    // 定期的な天気APIアクセス（海面気圧の取得）
    weatherService.update(nowMs);

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
        
        // --- 環境異常の警告出力 ---
        if (snap.environment.valid && snap.environment.enclosureWarning == core::EnclosureWarning::HeatTrapped) {
            float diff = snap.environment.scd41TemperatureC - snap.environment.temperatureC;
            services::Logger::warn("MAIN", "Enclosure Heat Trapped! Diff: +%.1f C", diff);
        }

        if (snap.ppg.state == core::PpgState::NoFinger) {
            services::Logger::info("MAIN", "PPG: No Finger");
        } else if (snap.ppg.state == core::PpgState::Calibrating) {
            services::Logger::info("MAIN", "PPG: Calibrating (IR: %u)", snap.ppg.ir);
        } else if (snap.ppg.state == core::PpgState::Measuring) {
            if (snap.ppg.signalPoor) {
                services::Logger::warn("MAIN", "PPG: Weak Signal (Adjust Pressure) [Amp: %u]", snap.ppg.signalAmplitude);
            } else if (snap.ppg.calculatedValid) {
                services::Logger::info("MAIN", "HR: %.1f (DPT: %.1f) bpm, SpO2: %.1f %% (DPT: %.1f %%), PI: %.1f %% [Amp: %u]",
                    snap.ppg.heartRateBpm, snap.ppg.dptHeartRateBpm,
                    snap.ppg.spo2Percent, snap.ppg.dptSpo2Percent,
                    snap.ppg.perfusionIndex, snap.ppg.signalAmplitude);
            } else {
                services::Logger::info("MAIN", "PPG: Measuring (Calc...) [Amp: %u]", snap.ppg.signalAmplitude);
            }
        }
    }

    static uint32_t previousSdLogMs = 0;
    // 5000ms (5秒) ごとにFRAMへ記録し、SDカードへフラッシュを試みる
    if (nowMs - previousSdLogMs >= 5000) {
        previousSdLogMs = nowMs;
        storageManager.appendRecord(sensorManager.snapshot(), nowMs);
        storageManager.flushPendingToSd();
    }
}