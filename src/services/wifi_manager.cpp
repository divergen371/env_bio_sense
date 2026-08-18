#include "services/wifi_manager.h"
#include "services/logger.h"
#include <ESPmDNS.h>

namespace services {

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
    startAP();
    isOn_ = true;
    Logger::info("WiFi", "Wi-Fi AP turned ON");
}

void WifiManager::turnOff() {
    if (!isOn_) return;
    stopAP();
    isOn_ = false;
    Logger::info("WiFi", "Wi-Fi AP turned OFF");
}

void WifiManager::startAP() {
    WiFi.mode(WIFI_AP);
    
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    
    WiFi.softAP("ENV_SENSE_AP", "12345678");
    
    // mDNS (Multicast DNS) の設定 (http://env.local でアクセス可能にする)
    if (MDNS.begin("env")) {
        Logger::info("WiFi", "mDNS responder started. You can access http://env.local");
    }
    
    Logger::info("WiFi", "AP Started. IP: %s", WiFi.softAPIP().toString().c_str());
}

void WifiManager::stopAP() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

} // namespace services
