#include "services/time_manager.h"
#include "services/logger.h"
#include "../config/secrets.h"
#include "hal/clock.h"
#include <WiFi.h>
#include <time.h>

namespace services {

bool TimeManager::enableNtpFallback_ = true;
uint32_t TimeManager::gnssGraceBeforeNtpMs_ = 60000;
bool TimeManager::ntpAttempted_ = false;
bool TimeManager::ntpRunning_ = false;
uint32_t TimeManager::ntpStartMs_ = 0;
bool TimeManager::ntpConnected_ = false;

void TimeManager::begin() {
    // 起動直後にはブロックしない
    Logger::info("NTP", "TimeManager initialized (NTP fallback enabled after %ums)", gnssGraceBeforeNtpMs_);
}

void TimeManager::update(uint32_t nowMs, core::TimeSource currentSource, GnssTimeSyncService* timeSyncService) {
    // すでにGNSSや手動で同期済みならNTPは試行しない
    if (currentSource == core::TimeSource::Gnss || currentSource == core::TimeSource::Manual) {
        return;
    }

    // 猶予期間が経過していない場合は何もしない
    if (nowMs < gnssGraceBeforeNtpMs_) {
        return;
    }

    if (!enableNtpFallback_ || ntpAttempted_) {
        return;
    }

    if (!ntpRunning_) {
        Logger::info("NTP", "GNSS not synced within grace period. Starting NTP fallback...");
        ntpRunning_ = true;
        ntpConnected_ = false;
        ntpStartMs_ = nowMs;

        // 非同期接続の開始
        WiFi.disconnect(true, true);
        delay(10);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        return;
    }

    // Wi-Fi接続待ち
    if (!ntpConnected_) {
        if (WiFi.status() == WL_CONNECTED) {
            ntpConnected_ = true;
            Logger::info("NTP", "Wi-Fi connected. Syncing time...");
            configTzTime("JST-9", "pool.ntp.org", "time.nist.gov");
        } else if (nowMs - ntpStartMs_ > 20000) {
            // 20秒経過しても接続できなければ失敗
            Logger::error("NTP", "Wi-Fi connection timeout");
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            ntpRunning_ = false;
            ntpAttempted_ = true;
        }
        return;
    }

    // NTP同期待ち
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        if (timeinfo.tm_year > 120) { // > 2020年
            // 同期成功
            char timeStringBuff[64];
            strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
            Logger::info("NTP", "Time synced successfully via NTP: %s", timeStringBuff);

            // UTC Epochを計算してGnssTimeSyncServiceへ報告
            time_t epoch = mktime(&timeinfo); // JST設定済みなのでローカルタイムとして扱われる
            // mktimeの挙動に注意。確実にUTCを求める場合
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            
            if (timeSyncService) {
                timeSyncService->reportNtpSync(tv.tv_sec, nowMs);
            } else {
                hal::Clock::setUtcAnchor((int64_t)tv.tv_sec * 1000000LL, hal::Clock::nowMonotonicUs(), core::TimeSource::Ntp);
            }

            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            ntpRunning_ = false;
            ntpAttempted_ = true;
        }
    } else if (nowMs - ntpStartMs_ > 30000) {
        // 接続後10秒(全体30秒)経過しても時刻が取れなければ失敗
        Logger::error("NTP", "NTP sync timeout");
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        ntpRunning_ = false;
        ntpAttempted_ = true;
    }
}

} // namespace services
