#include "hal/clock.h"
#include "utils/utc_time.h"
#include <sys/time.h>
#include <esp_timer.h>

namespace hal {

portMUX_TYPE Clock::mux_ = portMUX_INITIALIZER_UNLOCKED;
int64_t Clock::anchorUtcUs_ = 0;
int64_t Clock::anchorMonotonicUs_ = 0;
core::TimeSource Clock::source_ = core::TimeSource::Unset;
bool Clock::timeSet_ = false;
int64_t Clock::lastPpsMonotonicUs_ = 0;

int64_t Clock::nowMonotonicUs() {
    return esp_timer_get_time();
}

void Clock::setUtcAnchor(int64_t utcEpochUs, int64_t monotonicUs, core::TimeSource source) {
    portENTER_CRITICAL(&mux_);
    anchorUtcUs_ = utcEpochUs;
    anchorMonotonicUs_ = monotonicUs;
    source_ = source;
    timeSet_ = true;
    portEXIT_CRITICAL(&mux_);

    // 初回同期または大幅なズレがある場合はPOSIXシステム時計も合わせる
    // (JST等のタイムゾーンは考慮せず、システム時計は常にUTCで保持する)
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t posixUs = static_cast<int64_t>(tv.tv_sec) * 1000000LL + tv.tv_usec;
    int64_t diff = posixUs - utcEpochUs;
    if (diff < 0) diff = -diff;
    if (diff > 1000000LL) {
        tv.tv_sec = utcEpochUs / 1000000LL;
        tv.tv_usec = utcEpochUs % 1000000LL;
        settimeofday(&tv, nullptr);
    }
}

void Clock::reportPps(int64_t ppsMonotonicUs) {
    if (ppsMonotonicUs <= 0) return;
    portENTER_CRITICAL(&mux_);
    if (ppsMonotonicUs > lastPpsMonotonicUs_) {
        lastPpsMonotonicUs_ = ppsMonotonicUs;
    }
    portEXIT_CRITICAL(&mux_);
}

void Clock::enterHoldover() {
    portENTER_CRITICAL(&mux_);
    source_ = core::timeSourceAfterGnssLoss(source_, timeSet_);
    portEXIT_CRITICAL(&mux_);
}

int64_t Clock::utcEpochUsAt(int64_t monotonicUs) {
    int64_t aUtc, aMon;
    bool set;
    portENTER_CRITICAL(&mux_);
    aUtc = anchorUtcUs_;
    aMon = anchorMonotonicUs_;
    set = timeSet_;
    portEXIT_CRITICAL(&mux_);

    if (!set) {
        // 未同期の場合は単なるuptimeとして扱うか、またはフォールバック
        return monotonicUs;
    }

    return aUtc + (monotonicUs - aMon);
}

int64_t Clock::utcEpochMsAt(int64_t monotonicUs) {
    return utcEpochUsAt(monotonicUs) / 1000LL;
}

core::TimeSource Clock::source() {
    core::TimeSource s;
    portENTER_CRITICAL(&mux_);
    s = source_;
    portEXIT_CRITICAL(&mux_);
    return s;
}

bool Clock::isTimeSet() {
    bool set;
    portENTER_CRITICAL(&mux_);
    set = timeSet_;
    portEXIT_CRITICAL(&mux_);
    return set;
}

bool Clock::isDisciplined() {
    return core::isDisciplinedTimeSource(source());
}

core::TimeSnapshot Clock::snapshot() {
    core::TimeSnapshot result {};
    result.monotonicUs = nowMonotonicUs();

    int64_t anchorUtc;
    int64_t anchorMonotonic;
    int64_t lastPps;
    portENTER_CRITICAL(&mux_);
    anchorUtc = anchorUtcUs_;
    anchorMonotonic = anchorMonotonicUs_;
    lastPps = lastPpsMonotonicUs_;
    result.source = source_;
    result.utcValid = timeSet_;
    portEXIT_CRITICAL(&mux_);

    result.disciplined = core::isDisciplinedTimeSource(result.source);
    result.utcEpochUs = result.utcValid
        ? anchorUtc + (result.monotonicUs - anchorMonotonic) : 0;
    result.ppsAgeMs = utils::monotonicAgeMs(result.monotonicUs, lastPps);
    return result;
}

void Clock::markTimeSet() {
    portENTER_CRITICAL(&mux_);
    timeSet_ = true;
    portEXIT_CRITICAL(&mux_);
}

void Clock::setEpoch(time_t epoch) {
    int64_t nowMono = nowMonotonicUs();
    setUtcAnchor(static_cast<int64_t>(epoch) * 1000000LL, nowMono, core::TimeSource::Manual);
}

time_t Clock::getEpoch() {
    return utcEpochUsAt(nowMonotonicUs()) / 1000000LL;
}

String Clock::getFormattedDate() {
    time_t now = getEpoch() + (9 * 3600); // JST (+9h)
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d", 
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    return String(buf);
}

String Clock::getFormattedTime() {
    time_t now = getEpoch() + (9 * 3600); // JST (+9h)
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", 
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
}

} // namespace hal
