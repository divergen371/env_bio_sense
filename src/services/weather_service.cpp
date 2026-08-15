#include "services/weather_service.h"
#include "services/logger.h"
#include "config/secrets.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

namespace services {

WeatherService::WeatherService(SensorManager& sensorManager, WifiManager& wifiManager)
    : sensorManager_(sensorManager), wifiManager_(wifiManager) {
}

void WeatherService::begin() {
    lastFetchMs_ = 0;
    needsFetch_ = true;
    nearestStationId_ = "";
}

void WeatherService::update(uint32_t nowMs) {
    // APモード中（スマホ接続中）は自力でWi-Fiを使えないためスキップ
    if (wifiManager_.isOn()) {
        return;
    }
    
    // 20分（1200000ms）ごとに取得
    if (needsFetch_ || (nowMs - lastFetchMs_ > 1200000)) {
        if (fetchSeaLevelPressure()) {
            needsFetch_ = false;
            lastFetchMs_ = nowMs;
        } else {
            // 失敗した場合は5分後（300000ms）にリトライするよう調整
            lastFetchMs_ = nowMs - 1200000 + 300000;
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

bool WeatherService::fetchNearestStation() {
    Logger::info("Weather", "Fetching JMA AMeDAS station list...");
    
    WiFiClientSecure client;
    client.setInsecure(); // 気象庁API用のSSL検証をスキップ
    HTTPClient http;
    http.begin(client, "https://www.jma.go.jp/bosai/amedas/const/amedastable.json");
    
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Logger::error("Weather", "Failed to get amedastable. HTTP: %d", httpCode);
        http.end();
        return false;
    }
    
    WiFiClient* stream = http.getStreamPtr();
    
    float myLat = String(LOCATION_LAT).toFloat();
    float myLon = String(LOCATION_LON).toFloat();
    
    float minDistance = 999999.0f;
    String bestStation = "";
    
    int braceLevel = 0;
    String objBuffer = "";
    String currentStation = "";
    
    uint8_t buff[128];
    int len = http.getSize();
    uint32_t lastDataTime = millis();
    bool insideRootKey = false;
    
    // バッファリング付きの手作りストリームパーサー
    while((http.connected() || stream->available()) && (millis() - lastDataTime < 15000)) {
        size_t size = stream->available();
        if (size > 0) {
            lastDataTime = millis();
            int c = stream->readBytes(buff, (size > sizeof(buff) ? sizeof(buff) : size));
            
            for (int i = 0; i < c; i++) {
                char ch = (char)buff[i];
                
                if (braceLevel == 1 && ch == '"' && !insideRootKey) {
                    insideRootKey = true;
                    currentStation = "";
                } else if (braceLevel == 1 && ch == '"' && insideRootKey) {
                    insideRootKey = false;
                } else if (insideRootKey) {
                    currentStation += ch;
                }
                
                if (ch == '{') braceLevel++;
                if (ch == '}') braceLevel--;
                
                if (braceLevel >= 2) {
                    objBuffer += ch;
                } else if (braceLevel == 1 && ch == '}' && objBuffer.length() > 0) {
                    if (currentStation.length() == 5 && isDigit(currentStation[0])) {
                        bool hasPressure = false;
                        int typeIdx = objBuffer.indexOf("\"type\":\"");
                        if (typeIdx >= 0) {
                            char t = objBuffer.charAt(typeIdx + 8);
                            if (t == 'A' || t == 'B') hasPressure = true;
                        }
                        
                        if (hasPressure) {
                            int latIdx = objBuffer.indexOf("\"lat\":[");
                            int lonIdx = objBuffer.indexOf("\"lon\":[");
                            if (latIdx >= 0 && lonIdx >= 0) {
                                int latEnd = objBuffer.indexOf(']', latIdx);
                                int lonEnd = objBuffer.indexOf(']', lonIdx);
                                String latStr = objBuffer.substring(latIdx + 7, latEnd);
                                String lonStr = objBuffer.substring(lonIdx + 7, lonEnd);
                                
                                int commaLat = latStr.indexOf(',');
                                float lat = latStr.substring(0, commaLat).toFloat() + latStr.substring(commaLat + 1).toFloat() / 60.0f;
                                
                                int commaLon = lonStr.indexOf(',');
                                float lon = lonStr.substring(0, commaLon).toFloat() + lonStr.substring(commaLon + 1).toFloat() / 60.0f;
                                
                                float dx = lon - myLon;
                                float dy = lat - myLat;
                                float distSq = dx*dx + dy*dy;
                                
                                if (distSq < minDistance) {
                                    minDistance = distSq;
                                    bestStation = currentStation;
                                }
                            }
                        }
                    }
                    objBuffer = "";
                    currentStation = "";
                }
            }
            if(len > 0) {
                len -= c;
                if(len <= 0) break;
            }
        } else {
            delay(1);
        }
        
        // 全体を囲う '{' と '}' が閉じられたら（braceLevel==0に戻ったら）終了
        if (braceLevel == 0 && bestStation != "") {
            break;
        }
    }
    http.end();
    
    if (bestStation != "") {
        nearestStationId_ = bestStation;
        Logger::info("Weather", "Found nearest AMeDAS station: %s", bestStation.c_str());
        return true;
    }
    
    return false;
}

String WeatherService::fetchLatestTime() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://www.jma.go.jp/bosai/amedas/data/latest_time.txt");
    int httpCode = http.GET();
    String latest = "";
    if (httpCode == HTTP_CODE_OK) {
        latest = http.getString();
        latest.trim(); // "2026-08-14T02:40:00+09:00"
        
        // フォーマット変換 "YYYYMMDDHHMM00"
        latest.replace("-", "");
        latest.replace("T", "");
        latest.replace(":", "");
        if (latest.length() >= 14) {
            latest = latest.substring(0, 14);
        }
    }
    http.end();
    return latest;
}

bool WeatherService::fetchSeaLevelPressure() {
    Logger::info("Weather", "Fetching Sea Level Pressure from JMA AMeDAS...");
    
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
    
    bool success = false;
    
    if (nearestStationId_ == "") {
        if (!fetchNearestStation()) {
            Logger::error("Weather", "Failed to find nearest station");
            goto cleanup;
        }
    }
    
    {
        String dt = fetchLatestTime();
        if (dt == "" || dt.length() < 14) {
            Logger::error("Weather", "Failed to get latest_time");
            goto cleanup;
        }
        
        Logger::info("Weather", "Latest AMeDAS time: %s", dt.c_str());
        String url = "https://www.jma.go.jp/bosai/amedas/data/map/" + dt + ".json";
        
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, url);
        int httpCode = http.GET();
        
        if (httpCode == HTTP_CODE_OK) {
            WiFiClient* stream = http.getStreamPtr();
            int braceLevel = 0;
            String objBuffer = "";
            String currentStation = "";
            
            uint8_t buff[128];
            int len = http.getSize();
            uint32_t lastDataTime = millis();
            bool insideRootKey = false;
            
            while((http.connected() || stream->available()) && (millis() - lastDataTime < 15000)) {
                size_t size = stream->available();
                if (size > 0) {
                    lastDataTime = millis();
                    int c = stream->readBytes(buff, (size > sizeof(buff) ? sizeof(buff) : size));
                    
                    for (int i = 0; i < c; i++) {
                        char ch = (char)buff[i];
                        
                        if (braceLevel == 1 && ch == '"' && !insideRootKey) {
                            insideRootKey = true;
                            currentStation = "";
                        } else if (braceLevel == 1 && ch == '"' && insideRootKey) {
                            insideRootKey = false;
                        } else if (insideRootKey) {
                            currentStation += ch;
                        }
                        
                        if (ch == '{') braceLevel++;
                        if (ch == '}') braceLevel--;
                        
                        if (braceLevel >= 2 && currentStation == nearestStationId_) {
                            objBuffer += ch;
                        } else if (braceLevel == 1 && ch == '}') {
                            if (currentStation == nearestStationId_ && objBuffer.length() > 0) {
                                // "normalPressure":[1011.2,0] を探す
                                int npIdx = objBuffer.indexOf("\"normalPressure\":[");
                                if (npIdx >= 0) {
                                    int npEnd = objBuffer.indexOf(',', npIdx + 18);
                                    String npStr = objBuffer.substring(npIdx + 18, npEnd);
                                    float slp = npStr.toFloat();
                                    if (slp > 800.0f && slp < 1100.0f) {
                                        Logger::info("Weather", "Success! Sea Level Pressure: %.1f hPa (Station: %s)", slp, nearestStationId_.c_str());
                                        sensorManager_.setSeaLevelPressure(slp);
                                        success = true;
                                    }
                                }
                                goto request_done; // 目的の観測所が見つかったら早期終了
                            }
                            objBuffer = "";
                            currentStation = "";
                        }
                    }
                    if(len > 0) {
                        len -= c;
                        if(len <= 0) break;
                    }
                } else {
                    delay(1);
                }
            }
request_done:
            ; // empty statement
        } else {
            Logger::error("Weather", "HTTP GET map json failed: %d", httpCode);
        }
        http.end();
    }

cleanup:
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    return success;
}

} // namespace services
