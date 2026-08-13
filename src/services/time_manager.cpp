#include "services/time_manager.h"
#include "services/logger.h"
#include "../config/secrets.h"
#include "hal/clock.h"
#include <WiFi.h>
#include <time.h>

namespace services {

void TimeManager::begin() {
    Logger::info("NTP", "Starting Wi-Fi connection...");
    
    // ESP32のWi-Fiスタックを確実にリセットするため、一度切断する
    WiFi.disconnect(true, true);
    delay(100);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Wi-Fi接続を待機（ルーターによってはIP取得に時間がかかるため20秒に延長）
    uint32_t startAttempt = millis();
    bool connected = false;
    while (millis() - startAttempt < 20000) {
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (connected) {
        Logger::info("NTP", "Wi-Fi connected. IP: %s", WiFi.localIP().toString().c_str());
        
        // 日本標準時(JST)を設定し、NTPサーバーへ同期
        Logger::info("NTP", "Syncing time...");
        configTzTime("JST-9", "pool.ntp.org", "time.nist.gov");

        // 時刻が同期されるのを待機（最大10秒）
        struct tm timeinfo;
        bool timeSynced = false;
        startAttempt = millis();
        while (millis() - startAttempt < 10000) {
            // getLocalTime() は同期が完了していれば true を返す
            if (getLocalTime(&timeinfo, 10)) {
                // 1970年の初期時刻(年が70)から更新されていれば成功とみなす
                if (timeinfo.tm_year > 120) { // > 2020年
                    timeSynced = true;
                    break;
                }
            }
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        if (timeSynced) {
            char timeStringBuff[64];
            strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
            Logger::info("NTP", "Time synced successfully: %s", timeStringBuff);
            
            // hal::Clock 側に時刻設定完了を通知
            hal::Clock::markTimeSet();
        } else {
            Logger::error("NTP", "Failed to sync time via NTP.");
        }
    } else {
        Logger::error("NTP", "Failed to connect to Wi-Fi. Check SSID/PASSWORD in config/secrets.h");
    }

    // センサ計測に干渉しないようにWi-FiをOFFにする
    Logger::info("NTP", "Disconnecting Wi-Fi to reduce noise and power consumption.");
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
}

} // namespace services
