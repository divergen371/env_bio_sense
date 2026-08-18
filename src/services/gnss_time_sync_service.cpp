#include "services/gnss_time_sync_service.h"
#include "services/logger.h"

namespace services {

void GnssTimeSyncService::update(const core::GnssData& gnss, uint32_t nowMs) {
    if (!gnss.timeValid || gnss.lastPpsMonotonicUs == 0) {
        return;
    }

    // すでに処理済みのPPS時刻ならスキップ（NMEAは1秒に1回しか更新されないため）
    if (gnss.lastPpsMonotonicUs == lastPpsMonotonicUs_) {
        return;
    }

    // NMEAを受信した今、対応するPPSがどれくらい前に発生したかをチェック
    // 正常なら50〜200ms程度。
    int64_t latencyUs = hal::Clock::nowMonotonicUs() - gnss.lastPpsMonotonicUs;

    // もしレイテンシが0未満や1秒以上なら、PPSとNMEAの対応が崩れている
    if (latencyUs < 0 || latencyUs > 900000LL) {
        services::Logger::warn("TimeSync", "NMEA arrived too late or PPS is wrong (latency: %lld us)", latencyUs);
        consecutiveMatches_ = 0;
        return;
    }

    // PPS間の間隔をチェック
    if (lastPpsMonotonicUs_ != 0) {
        int64_t interval = gnss.lastPpsMonotonicUs - lastPpsMonotonicUs_;
        if (interval < 900000LL || interval > 1100000LL) {
            services::Logger::warn("TimeSync", "PPS interval out of bounds: %lld us", interval);
            consecutiveMatches_ = 0;
        }
    }
    lastPpsMonotonicUs_ = gnss.lastPpsMonotonicUs;

    // RMCの時刻はミリ秒単位だが、PPSエッジは常に「.000秒」のはず
    int64_t utcEpochUs = (gnss.utcEpochMs / 1000LL) * 1000000LL;

    // 新しいアンカー候補
    UtcAnchor candidate;
    candidate.ppsMonotonicUs = gnss.lastPpsMonotonicUs;
    candidate.ppsUtcEpochUs = utcEpochUs;
    candidate.valid = true;

    if (lastAnchor_.valid) {
        int64_t expectedUtc = lastAnchor_.ppsUtcEpochUs + 1000000LL;
        if (utcEpochUs == expectedUtc) {
            consecutiveMatches_++;
        } else {
            services::Logger::warn("TimeSync", "UTC mismatch! Expected: %lld, Got: %lld", expectedUtc, utcEpochUs);
            consecutiveMatches_ = 0;
        }
    } else {
        services::Logger::info("TimeSync", "First valid PPS+NMEA anchor candidate registered");
        consecutiveMatches_ = 1;
    }

    lastAnchor_ = candidate;

    // 連続して3回一致したら同期確立
    if (consecutiveMatches_ >= 3) {
        establishGnssSync(candidate.ppsUtcEpochUs, candidate.ppsMonotonicUs);
    }
}

void GnssTimeSyncService::establishGnssSync(int64_t utcUs, int64_t monotonicUs) {
    if (state_ != core::TimeSource::Gnss) {
        services::Logger::info("TimeSync", "GNSS Time Sync Established");
        state_ = core::TimeSource::Gnss;
    }
    hal::Clock::setUtcAnchor(utcUs, monotonicUs, core::TimeSource::Gnss);
}

void GnssTimeSyncService::reportHoldover(uint32_t nowMs) {
    if (state_ == core::TimeSource::Gnss) {
        services::Logger::warn("TimeSync", "GNSS Time Sync Lost -> Holdover");
        state_ = core::TimeSource::Holdover;
    }
    consecutiveMatches_ = 0;
}

void GnssTimeSyncService::reportNtpSync(int64_t ntpUtcEpochMs, uint32_t nowMs) {
    if (state_ == core::TimeSource::Unset || state_ == core::TimeSource::Holdover) {
        services::Logger::info("TimeSync", "NTP Time Sync Applied");
        state_ = core::TimeSource::Ntp;
        hal::Clock::setUtcAnchor(ntpUtcEpochMs * 1000LL, hal::Clock::nowMonotonicUs(), core::TimeSource::Ntp);
    }
}

} // namespace services
