#include "services/wifi_manager.h"
#include "services/logger.h"
#include "config/secrets.h"
#include <ESPmDNS.h>

namespace services {

static_assert(sizeof(WEB_ADMIN_PASSWORD) - 1 >= 12,
              "WEB_ADMIN_PASSWORD must contain at least 12 characters");

WifiManager::WifiManager() : isOn_(false) {}

void WifiManager::begin() {
    // LwIPスタックを確実に初期化するため、一度STAモードにする
    WiFi.mode(WIFI_STA);
    WiFi.mode(WIFI_OFF);
    isOn_ = false;
}

void WifiManager::toggle() {
    if (isOn_) {
        turnOff();
    } else {
        turnOn();
    }
}

void WifiManager::update() {
    // 現在は特にバックグラウンド処理はなし
}

void WifiManager::turnOn() {
    if (isOn_) return;
    isOn_ = startAP();
    if (isOn_) Logger::info("WiFi", "Wi-Fi AP turned ON");
}

void WifiManager::turnOff() {
    if (!isOn_) return;
    stopAP();
    isOn_ = false;
    Logger::info("WiFi", "Wi-Fi AP turned OFF");
}

bool WifiManager::startAP() {
    WiFi.mode(WIFI_AP);
    
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    
    if (!WiFi.softAP(WEB_AP_SSID, WEB_ADMIN_PASSWORD)) {
        Logger::error("WiFi", "Failed to start protected AP");
        WiFi.mode(WIFI_OFF);
        return false;
    }
    
    // mDNS (Multicast DNS) の設定 (http://env.local でアクセス可能にする)
    if (MDNS.begin("env")) {
        Logger::info("WiFi", "mDNS responder started. You can access http://env.local");
    }
    
    Logger::info("WiFi", "Protected AP %s started. IP: %s",
                 WEB_AP_SSID, WiFi.softAPIP().toString().c_str());
    return true;
}

void WifiManager::stopAP() {
    MDNS.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

} // namespace services
