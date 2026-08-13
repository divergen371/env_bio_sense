#pragma once

#include <Arduino.h>
#include <time.h>

namespace hal {

class Clock {
public:
    static void setEpoch(time_t epoch);
    static time_t getEpoch();
    static bool isTimeSet();
    static void markTimeSet();
    
    // Returns YYYYMMDD (e.g. "20231024")
    static String getFormattedDate();
    
    // Returns HH:MM:SS (e.g. "14:30:00")
    static String getFormattedTime();

private:
    static bool timeSet_;
};

} // namespace hal
