#pragma once

#include "drivers/sensors/lc76g_sensor.h"
#include "hal/clock.h"
#include <cstdint>

namespace services {

class GnssTimeSyncService {
public:
    void update(const core::GnssData& gnss, uint32_t nowMs);
    void reportHoldover(uint32_t nowMs);
    void reportNtpSync(int64_t ntpUtcEpochMs, uint32_t nowMs);

    core::TimeSource currentState() const { return state_; }

private:
    struct UtcAnchor {
        int64_t ppsMonotonicUs {};
        int64_t ppsUtcEpochUs {};
        uint32_t ppsSequence {};
        bool valid {};
    };

    UtcAnchor lastAnchor_ {};
    core::TimeSource state_ {core::TimeSource::Unset};

    uint32_t consecutiveMatches_ {0};
    int64_t lastPpsMonotonicUs_ {0};

    void establishGnssSync(int64_t utcUs, int64_t monotonicUs);
};

} // namespace services
