#pragma once

#include <cstdint>
#include "core/sensor_types.h"
#include "services/gnss_time_sync_service.h"

namespace services {

class TimeManager {
public:
    static void begin();
    static void update(uint32_t nowMs, core::TimeSource currentSource, GnssTimeSyncService* timeSyncService);

private:
    static bool enableNtpFallback_;
    static uint32_t gnssGraceBeforeNtpMs_;
    static bool ntpAttempted_;
    static bool ntpRunning_;
    static uint32_t ntpStartMs_;
    static bool ntpConnected_;
};

} // namespace services
