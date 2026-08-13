#include "services/weather_service.h"
#include "services/logger.h"
#include "config/secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace services {

WeatherService::WeatherService(SensorManager& sensorManager, WifiManager& wifiManager)
    : sensorManager_(sensorManager), wifiManager_(wifiManager) {
}

void WeatherService::begin() {
    lastFetchMs_ = 0;
    needsFetch_ = true;
}

void WeatherService::update(uint32_t nowMs) {
    // APモード中（スマホ接続中）は自力でWi-Fiを使えないためスキップ
    if (wifiManager_.isOn()) {
        return;
    }
    
    // 1時間（3600000ms）ごとに取得
    if (needsFetch_ || (nowMs - lastFetchMs_ > 3600000)) {
        if (fetchSeaLevelPressure()) {
            lastFetchMs_ = nowMs;
            needsFetch_ = false;
        } else {
            // 失敗した場合は 5 分後にリトライ
            lastFetchMs_ = nowMs - 3600000 + 300000;
        }
    }
}

void WeatherService::forceUpdate(float pressureHpa) {
    Logger::info("Weather", "Forcing Sea Level Pressure update from API: %.1f hPa", pressureHpa);
    sensorManager_.setSeaLevelPressure(pressureHpa);
    
    // 自力での取得タイマーもリセット
    lastFetchMs_ = millis();
    needsFetch_ = false;
}

bool WeatherService::fetchSeaLevelPressure() {
    Logger::info("Weather", "Fetching Sea Level Pressure from Open-Meteo...");
    
    // 既存のWi-Fi状態を退避
    WiFi.disconnect(true, true);
    delay(100);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    uint32_t startAttempt = millis();
    bool connected = false;
    while (millis() - startAttempt < 15000) {
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        delay(500);
    }
    
    if (!connected) {
        Logger::error("Weather", "Failed to connect to Wi-Fi");
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        return false;
    }
    
    String url = "http://api.open-meteo.com/v1/forecast?latitude=";
    url += LOCATION_LAT;
    url += "&longitude=";
    url += LOCATION_LON;
    url += "&current=pressure_msl";
    
    HTTPClient http;
    http.begin(url);
    int httpCode = http.GET();
    
    bool success = false;
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error) {
            if (doc["current"]["pressure_msl"].is<float>()) {
                float slp = doc["current"]["pressure_msl"];
                Logger::info("Weather", "Success! Sea Level Pressure: %.1f hPa", slp);
                sensorManager_.setSeaLevelPressure(slp);
                success = true;
            }
        } else {
            Logger::error("Weather", "JSON Parse failed: %s", error.c_str());
        }
    } else {
        Logger::error("Weather", "HTTP GET failed, error: %s", http.errorToString(httpCode).c_str());
    }
    
    http.end();
    
    // Wi-Fi をオフに戻す
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    
    return success;
}

} // namespace services
