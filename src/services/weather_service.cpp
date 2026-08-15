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
    numCachedStations_ = 0;
}

void WeatherService::update(uint32_t nowMs) {
    // APモード中（スマホ接続中）は自力でWi-Fiを使えないためスキップ
    if (wifiManager_.isOn()) {
        return;
    }
    
    // 15分（900000ms）ごとに取得
    if (needsFetch_ || (nowMs - lastFetchMs_ > 900000)) {
        if (fetchSeaLevelPressure()) {
            needsFetch_ = false;
            lastFetchMs_ = nowMs;
        } else {
            // 失敗した場合は5分後（300000ms）にリトライするよう調整
            lastFetchMs_ = nowMs - 900000 + 300000;
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

bool WeatherService::fetchNearestStations() {
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
    
    numCachedStations_ = 0;
    
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
                                
                                // Insert into sorted array
                                if (numCachedStations_ < 5 || distSq < nearestStations_[4].distSq) {
                                    int pos = numCachedStations_ < 5 ? numCachedStations_ : 4;
                                    while (pos > 0 && nearestStations_[pos-1].distSq > distSq) {
                                        if (pos < 5) nearestStations_[pos] = nearestStations_[pos-1];
                                        pos--;
                                    }
                                    if (pos < 5) {
                                        nearestStations_[pos].id = currentStation;
                                        nearestStations_[pos].distSq = distSq;
                                        if (numCachedStations_ < 5) numCachedStations_++;
                                    }
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
        
        if (braceLevel == 0 && numCachedStations_ > 0) {
            break;
        }
    }
    http.end();
    
    if (numCachedStations_ > 0) {
        Logger::info("Weather", "Found %d AMeDAS stations.", numCachedStations_);
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
    
    if (numCachedStations_ == 0) {
        if (!fetchNearestStations()) {
            Logger::error("Weather", "Failed to find nearest stations");
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
            
            float validPressures[5] = {0};
            int readCount = 0;
            
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
                            bool isTarget = false;
                            for (int k = 0; k < numCachedStations_; k++) {
                                if (currentStation == nearestStations_[k].id) {
                                    isTarget = true;
                                    break;
                                }
                            }
                            if (isTarget) {
                                objBuffer += ch;
                            }
                        } else if (braceLevel == 1 && ch == '}') {
                            int targetIdx = -1;
                            for (int k = 0; k < numCachedStations_; k++) {
                                if (currentStation == nearestStations_[k].id) {
                                    targetIdx = k;
                                    break;
                                }
                            }
                            
                            if (targetIdx >= 0 && objBuffer.length() > 0) {
                                // "normalPressure":[1011.2,0] を探す
                                int npIdx = objBuffer.indexOf("\"normalPressure\":[");
                                if (npIdx >= 0) {
                                    int npEnd = objBuffer.indexOf(',', npIdx + 18);
                                    String npStr = objBuffer.substring(npIdx + 18, npEnd);
                                    float slp = npStr.toFloat();
                                    if (slp > 800.0f && slp < 1100.0f) {
                                        validPressures[targetIdx] = slp;
                                        readCount++;
                                    }
                                }
                            }
                            objBuffer = "";
                            currentStation = "";
                            
                            if (readCount >= numCachedStations_) {
                                goto request_done;
                            }
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
            if (readCount > 0) {
                float sumWeights = 0.0f;
                float sumWPressures = 0.0f;
                int usedCount = 0;
                float minP = 9999.0f, maxP = -9999.0f;
                float minDistSq = 999999.0f, maxDistSq = 0.0f;
                
                for (int k = 0; k < numCachedStations_; k++) {
                    if (validPressures[k] > 0.0f) {
                        float dSq = nearestStations_[k].distSq;
                        if (dSq < 1e-6f) { // Very close, handle div-by-zero
                            sumWPressures = validPressures[k];
                            sumWeights = 1.0f;
                            usedCount = 1;
                            minP = maxP = validPressures[k];
                            minDistSq = maxDistSq = 0.0f;
                            break; 
                        } else {
                            float w = 1.0f / dSq;
                            sumWeights += w;
                            sumWPressures += w * validPressures[k];
                            usedCount++;
                            if (validPressures[k] < minP) minP = validPressures[k];
                            if (validPressures[k] > maxP) maxP = validPressures[k];
                            if (dSq < minDistSq) minDistSq = dSq;
                            if (dSq > maxDistSq) maxDistSq = dSq;
                        }
                    }
                }
                
                if (usedCount > 0 && sumWeights > 0.0f) {
                    float interpolatedSlp = sumWPressures / sumWeights;
                    
                    if (interpolatedSlp < minP) interpolatedSlp = minP;
                    if (interpolatedSlp > maxP) interpolatedSlp = maxP;
                    
                    float minDistKm = sqrt(minDistSq) * 111.0f;
                    float maxDistKm = sqrt(maxDistSq) * 111.0f;
                    
                    core::PressureFieldState state = (usedCount >= 3) ? core::PressureFieldState::Valid : core::PressureFieldState::LastKnown;
                    Logger::info("Weather", "IDW Success! SLP: %.1f hPa (Used %d/%d stations, MinDist: %.1fkm, MaxDist: %.1fkm), State: %d", 
                        interpolatedSlp, usedCount, numCachedStations_, minDistKm, maxDistKm, (int)state);
                    sensorManager_.setSeaLevelPressure(interpolatedSlp, state);
                    success = true;
                }
            }
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
