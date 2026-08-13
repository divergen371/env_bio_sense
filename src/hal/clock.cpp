#include "hal/clock.h"
#include <sys/time.h>

namespace hal {

bool Clock::timeSet_ = false;

void Clock::setEpoch(time_t epoch) {
    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    timeSet_ = true;
}

time_t Clock::getEpoch() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec;
}

bool Clock::isTimeSet() {
    return timeSet_;
}

void Clock::markTimeSet() {
    timeSet_ = true;
}

String Clock::getFormattedDate() {
    time_t now = getEpoch();
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d", 
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    return String(buf);
}

String Clock::getFormattedTime() {
    time_t now = getEpoch();
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", 
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
}

} // namespace hal
