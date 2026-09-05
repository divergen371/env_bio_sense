#pragma once

#include "core/sensor_types.h"
#include "utils/location_policy.h"

namespace utils {

class LocationSelector {
public:
    void reset(const core::DeviceLocation& configured) {
        live_ = {};
        lastKnown_ = {};
        configured_ = configured;
        liveCapturedMs_ = 0;
        lastKnownCapturedMs_ = 0;
        candidateLatDeg_ = 0.0;
        candidateLonDeg_ = 0.0;
        candidateCount_ = 0;
        hasCandidate_ = false;
    }

    void update(const core::GnssData& gnss, uint32_t nowMs) {
        if (!location_policy::gnssFixAcceptable(gnss)) {
            live_.valid = false;
            hasCandidate_ = false;
            candidateCount_ = 0;
            return;
        }
        if (live_.valid) {
            const double movementM = location_policy::distanceMeters(
                live_.latitudeDeg, live_.longitudeDeg,
                gnss.latitudeDeg, gnss.longitudeDeg);
            if (std::isfinite(movementM) &&
                movementM <= location_policy::CANDIDATE_MAX_SPREAD_M) {
                assignLive(gnss, nowMs);
                return;
            }
            // A jump beyond the acceptance radius must be confirmed as a new
            // cluster; retain the prior fix only as LastKnown meanwhile.
            live_.valid = false;
        }
        if (!hasCandidate_) {
            candidateLatDeg_ = gnss.latitudeDeg;
            candidateLonDeg_ = gnss.longitudeDeg;
            candidateCount_ = 1;
            hasCandidate_ = true;
            return;
        }

        const double spreadM = location_policy::distanceMeters(
            candidateLatDeg_, candidateLonDeg_,
            gnss.latitudeDeg, gnss.longitudeDeg);
        if (std::isfinite(spreadM) &&
            spreadM <= location_policy::CANDIDATE_MAX_SPREAD_M) {
            if (candidateCount_ != UINT8_MAX) ++candidateCount_;
        } else {
            candidateLatDeg_ = gnss.latitudeDeg;
            candidateLonDeg_ = gnss.longitudeDeg;
            candidateCount_ = 1;
        }
        if (candidateCount_ >= location_policy::CANDIDATE_CONFIRMATIONS) {
            assignLive(gnss, nowMs);
            hasCandidate_ = false;
            candidateCount_ = 0;
        }
    }

    core::DeviceLocation current(uint32_t nowMs) const {
        if (live_.valid) {
            core::DeviceLocation result = live_;
            result.ageMs = nowMs - liveCapturedMs_;
            if (result.ageMs <= location_policy::GNSS_LIVE_MAX_AGE_MS) {
                result.source = core::LocationSource::GnssLive;
                return result;
            }
        }
        if (lastKnown_.valid) {
            core::DeviceLocation result = lastKnown_;
            result.ageMs = nowMs - lastKnownCapturedMs_;
            if (result.ageMs <= location_policy::LAST_KNOWN_MAX_AGE_MS) {
                result.source = core::LocationSource::GnssLastKnown;
                return result;
            }
        }
        if (configured_.valid) return configured_;
        return {};
    }

private:
    void assignLive(const core::GnssData& gnss, uint32_t nowMs) {
        const uint32_t capturedMs = nowMs - gnss.ageMs;
        live_.latitudeDeg = gnss.latitudeDeg;
        live_.longitudeDeg = gnss.longitudeDeg;
        live_.source = core::LocationSource::GnssLive;
        live_.ageMs = gnss.ageMs;
        live_.capturedMonotonicUs = gnss.sampleMonotonicUs > 0
            ? gnss.sampleMonotonicUs
            : static_cast<int64_t>(capturedMs) * 1000LL;
        live_.satellites = gnss.satellites;
        live_.hdop = gnss.hdop;
        live_.valid = true;
        liveCapturedMs_ = capturedMs;
        lastKnown_ = live_;
        lastKnown_.source = core::LocationSource::GnssLastKnown;
        lastKnownCapturedMs_ = capturedMs;
    }

    core::DeviceLocation live_ {};
    core::DeviceLocation lastKnown_ {};
    core::DeviceLocation configured_ {};
    uint32_t liveCapturedMs_ {};
    uint32_t lastKnownCapturedMs_ {};
    double candidateLatDeg_ {};
    double candidateLonDeg_ {};
    uint8_t candidateCount_ {};
    bool hasCandidate_ {false};
};

} // namespace utils
