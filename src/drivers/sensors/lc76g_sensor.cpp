#include "drivers/sensors/lc76g_sensor.h"
#include "hal/pins.h"
#include "services/logger.h"
#include <esp_timer.h>

namespace drivers {
namespace sensors {

namespace {
    // タイムアウト閾値
    constexpr uint32_t NMEA_ALIVE_TIMEOUT_MS = 2500;
    constexpr uint32_t FIX_STALE_TIMEOUT_MS  = 3000;
    constexpr uint32_t UTC_STALE_TIMEOUT_MS  = 3000;
    constexpr uint32_t PPS_RECENT_TIMEOUT_MS = 2500;

    // PPS割り込み用共有データ (ISRとメインループ間で共有)
    volatile uint32_t ppsSequenceCounter = 0;
    volatile int64_t lastPpsMonotonicUs = 0;
    volatile bool hasNewPpsEvent = false;
    portMUX_TYPE ppsMux = portMUX_INITIALIZER_UNLOCKED;
}

void IRAM_ATTR Lc76gSensor::onPps() {
    int64_t nowUs = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&ppsMux);
    ppsSequenceCounter++;
    lastPpsMonotonicUs = nowUs;
    hasNewPpsEvent = true;
    portEXIT_CRITICAL_ISR(&ppsMux);
}

Lc76gSensor::Lc76gSensor(HardwareSerial& serial)
    : serial_(serial) {
}

bool Lc76gSensor::begin() {
    status_.transportState = core::DeviceState::Initializing;
    
    // RXバッファ拡張
    serial_.setRxBufferSize(2048);
    
    // ボーレート判定処理開始 (115200から試す)
    isBaudProbing_ = true;
    probeStep_ = 0;
    baudRate_ = 115200;
    baudProbeStartMs_ = millis();
    
    serial_.begin(baudRate_, SERIAL_8N1, hal::pins::GNSS_UART_RX, hal::pins::GNSS_UART_TX);
    services::Logger::info("Lc76gSensor", "Probing GNSS UART at 115200 bps");

    // PPS初期化
    pinMode(hal::pins::GNSS_PPS, INPUT);
    attachInterrupt(digitalPinToInterrupt(hal::pins::GNSS_PPS), onPps, RISING);

    return true; // 非ブロッキングで進む
}

void Lc76gSensor::update(uint32_t nowMs) {
    if (isBaudProbing_) {
        probeBaudRate(nowMs);
    } else {
        processNmea(nowMs);
    }
    
    updateState(nowMs);
}

void Lc76gSensor::probeBaudRate(uint32_t nowMs) {
    size_t budget = 512;
    while (budget-- > 0 && serial_.available() > 0) {
        gps_.encode(static_cast<char>(serial_.read()));
    }

    if (gps_.sentencesWithFix() > 0 || gps_.passedChecksum() > 0) {
        // NMEAが正しく受信できた
        isBaudProbing_ = false;
        status_.uartBaud = baudRate_;
        status_.transportState = core::DeviceState::Ready;
        services::Logger::info("Lc76gSensor", "NMEA detected at %u bps", baudRate_);
        return;
    }

    if (nowMs - baudProbeStartMs_ > 3000) {
        // 3秒経過してもパース成功しなければ次へ
        if (probeStep_ == 0) {
            probeStep_ = 1;
            baudRate_ = 9600;
            baudProbeStartMs_ = nowMs;
            serial_.updateBaudRate(baudRate_);
            services::Logger::info("Lc76gSensor", "Probing GNSS UART at 9600 bps");
        } else {
            // 両方だめならとりあえず115200に戻してリトライし続ける
            probeStep_ = 0;
            baudRate_ = 115200;
            baudProbeStartMs_ = nowMs;
            serial_.updateBaudRate(baudRate_);
            services::Logger::warn("Lc76gSensor", "Baud rate probe failed, retrying 115200 bps");
        }
    }
}

void Lc76gSensor::processNmea(uint32_t nowMs) {
    size_t budget = 512;
    uint32_t initialChecksumFailures = gps_.failedChecksum();
    bool parsedNewSentence = false;

    while (budget-- > 0 && serial_.available() > 0) {
        if (gps_.encode(static_cast<char>(serial_.read()))) {
            parsedNewSentence = true;
        }
    }

    uint32_t newChecksumFailures = gps_.failedChecksum();
    if (newChecksumFailures > initialChecksumFailures) {
        status_.checksumFailures += (newChecksumFailures - initialChecksumFailures);
    }

    if (parsedNewSentence) {
        lastValidNmeaMs_ = nowMs;
        status_.nmeaAlive = true;
        updateFixStatus(nowMs);
    }
}

void Lc76gSensor::updateFixStatus(uint32_t nowMs) {
    // 位置が更新され、かつ有効か？ (isUpdated()は前回encode()からの変更をチェック)
    if (gps_.location.isUpdated() && gps_.location.isValid()) {
        data_.latitudeDeg = gps_.location.lat();
        data_.longitudeDeg = gps_.location.lng();
        data_.sampleMonotonicUs = esp_timer_get_time();
        lastSuccessMs_ = nowMs;
        lastError_ = core::ErrorCode::None;
    }
    
    // タイムスタンプの更新
    if (gps_.time.isUpdated() && gps_.time.isValid() && gps_.date.isUpdated() && gps_.date.isValid()) {
        struct tm t = {0};
        t.tm_year = gps_.date.year() - 1900;
        t.tm_mon = gps_.date.month() - 1;
        t.tm_mday = gps_.date.day();
        t.tm_hour = gps_.time.hour();
        t.tm_min = gps_.time.minute();
        t.tm_sec = gps_.time.second();
        // C言語のtimegm相当
        time_t epoch = mktime(&t);
        // mktimeはローカルタイムと解釈するためTZ設定に注意。ここでは後でClockで統一的に扱うので仮にUTCとして保持
        data_.utcEpochMs = static_cast<int64_t>(epoch) * 1000 + gps_.time.centisecond() * 10;
        data_.timeValid = true;

        int64_t ppsCopy;
        portENTER_CRITICAL(&ppsMux);
        ppsCopy = lastPpsMonotonicUs;
        portEXIT_CRITICAL(&ppsMux);
        data_.lastPpsMonotonicUs = ppsCopy;
    }

    if (gps_.altitude.isUpdated() && gps_.altitude.isValid()) {
        data_.altitudeMslM = gps_.altitude.meters();
        data_.altitudeValid = true;
    }

    if (gps_.speed.isUpdated() && gps_.speed.isValid()) {
        data_.speedMps = gps_.speed.mps();
        data_.speedValid = true;
    }

    if (gps_.course.isUpdated() && gps_.course.isValid()) {
        data_.courseDeg = gps_.course.deg();
        data_.courseValid = true;
    }

    if (gps_.hdop.isUpdated() && gps_.hdop.isValid()) {
        data_.hdop = gps_.hdop.value() / 100.0f; // TinyGPSPlus hdop() is in 100ths
        data_.hdopValid = true;
    }

    if (gps_.satellites.isUpdated() && gps_.satellites.isValid()) {
        data_.satellites = gps_.satellites.value();
    }
}

void Lc76gSensor::updateState(uint32_t nowMs) {
    if (isBaudProbing_) return;

    // NMEA alive 判定
    if (nowMs - lastValidNmeaMs_ > NMEA_ALIVE_TIMEOUT_MS) {
        status_.nmeaAlive = false;
        status_.transportState = core::DeviceState::Offline;
    } else {
        status_.transportState = core::DeviceState::Ready;
    }
    status_.nmeaAgeMs = status_.nmeaAlive ? (nowMs - lastValidNmeaMs_) : UINT32_MAX;

    // Fix 判定
    if (gps_.location.isValid() && gps_.location.age() < FIX_STALE_TIMEOUT_MS) {
        data_.fixValid = true;
        status_.fixAgeMs = gps_.location.age();
    } else {
        data_.fixValid = false;
        status_.fixAgeMs = UINT32_MAX;
    }

    // 各種鮮度判定
    if (gps_.time.age() > UTC_STALE_TIMEOUT_MS || gps_.date.age() > UTC_STALE_TIMEOUT_MS) {
        data_.timeValid = false;
    }
    if (gps_.altitude.age() > FIX_STALE_TIMEOUT_MS) data_.altitudeValid = false;
    if (gps_.speed.age() > FIX_STALE_TIMEOUT_MS) data_.speedValid = false;
    if (gps_.course.age() > FIX_STALE_TIMEOUT_MS) data_.courseValid = false;
    if (gps_.hdop.age() > FIX_STALE_TIMEOUT_MS) data_.hdopValid = false;

    // PPS処理
    int64_t lastPpsCopy;
    uint32_t ppsCountCopy;
    portENTER_CRITICAL(&ppsMux);
    lastPpsCopy = lastPpsMonotonicUs;
    ppsCountCopy = ppsSequenceCounter;
    portEXIT_CRITICAL(&ppsMux);

    int64_t ppsAgeUs = esp_timer_get_time() - lastPpsCopy;
    if (ppsCountCopy > 0 && ppsAgeUs < PPS_RECENT_TIMEOUT_MS * 1000) {
        status_.ppsSeen = true;
        status_.ppsRecent = true;
        status_.ppsAgeMs = ppsAgeUs / 1000;
    } else {
        status_.ppsRecent = false;
        status_.ppsAgeMs = UINT32_MAX;
    }
    
    // データ全体のage
    data_.ageMs = status_.fixAgeMs;
}

bool Lc76gSensor::readGnss(core::GnssData& out, uint32_t nowMs) const {
    out = data_;
    return status_.transportState == core::DeviceState::Ready;
}

core::GnssStatus Lc76gSensor::status(uint32_t nowMs) const {
    return status_;
}

bool Lc76gSensor::takePpsEvent(PpsEvent& out) {
    bool hasEvent = false;
    portENTER_CRITICAL(&ppsMux);
    if (hasNewPpsEvent) {
        out.sequence = ppsSequenceCounter;
        out.monotonicUs = lastPpsMonotonicUs;
        hasNewPpsEvent = false;
        hasEvent = true;
    }
    portEXIT_CRITICAL(&ppsMux);
    return hasEvent;
}

} // namespace sensors
} // namespace drivers
