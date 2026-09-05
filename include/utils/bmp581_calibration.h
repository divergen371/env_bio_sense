#pragma once

#include "utils/altitude_math.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace utils {
namespace bmp581_calibration {

constexpr float REFERENCE_ALTITUDE_M = 13.6f;
constexpr uint32_t SETTLE_DURATION_MS = 60u * 1000u;
constexpr uint32_t COLLECTION_DURATION_MS = 5u * 60u * 1000u;
constexpr uint32_t SAMPLE_INTERVAL_MS = 100u;
constexpr uint32_t EXPECTED_SAMPLES =
    COLLECTION_DURATION_MS / SAMPLE_INTERVAL_MS;
constexpr float MIN_VALID_RATIO = 0.80f;
constexpr float MAX_OFFSET_HPA = 5.0f;
constexpr float MAX_MEAN_ERROR_M = 0.3f;
constexpr float MAX_STDDEV_M = 0.5f;

enum class Failure : uint8_t {
    None = 0,
    Busy = 1,
    InvalidReferenceAltitude = 2,
    PressureFieldNotValid = 3,
    InsufficientSamples = 4,
    InvalidStatistics = 5,
    OffsetOutOfRange = 6,
    MeanAltitudeOutOfRange = 7,
    AltitudeNoiseTooHigh = 8,
    PersistenceFailed = 9,
    Cancelled = 10
};

inline const char* failureName(Failure failure) {
    switch (failure) {
        case Failure::None: return "NONE";
        case Failure::Busy: return "BUSY";
        case Failure::InvalidReferenceAltitude:
            return "INVALID_REFERENCE_ALTITUDE";
        case Failure::PressureFieldNotValid:
            return "PRESSURE_FIELD_NOT_VALID";
        case Failure::InsufficientSamples: return "INSUFFICIENT_SAMPLES";
        case Failure::InvalidStatistics: return "INVALID_STATISTICS";
        case Failure::OffsetOutOfRange: return "OFFSET_OUT_OF_RANGE";
        case Failure::MeanAltitudeOutOfRange:
            return "MEAN_ALTITUDE_OUT_OF_RANGE";
        case Failure::AltitudeNoiseTooHigh:
            return "ALTITUDE_NOISE_TOO_HIGH";
        case Failure::PersistenceFailed: return "PERSISTENCE_FAILED";
        case Failure::Cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

struct Sample {
    float residualHpa {};
    float temperatureC {};
    float seaLevelPressureHpa {};

    Sample() = default;
    Sample(float residual, float temperature, float seaLevelPressure)
        : residualHpa(residual),
          temperatureC(temperature),
          seaLevelPressureHpa(seaLevelPressure) {}
};

struct Result {
    bool success {false};
    Failure failure {Failure::None};
    float pressureOffsetHpa {NAN};
    float meanTemperatureC {NAN};
    float meanSeaLevelPressureHpa {NAN};
    float meanRawPressureHpa {NAN};
    float meanExpectedPressureHpa {NAN};
    float preCalibrationMeanAltitudeM {NAN};
    float preCalibrationStdDevM {NAN};
    float postCalibrationMeanAltitudeM {NAN};
    float postCalibrationStdDevM {NAN};
    uint32_t totalSamples {};
    uint32_t validSamples {};
    uint32_t usedSamples {};
    uint32_t externalTemperatureSamples {};
};

class RunningStats {
public:
    void add(double value) {
        ++count_;
        const double delta = value - mean_;
        mean_ += delta / count_;
        m2_ += delta * (value - mean_);
    }

    uint32_t count() const { return count_; }
    float mean() const {
        return count_ > 0 ? static_cast<float>(mean_) : NAN;
    }
    float populationStdDev() const {
        return count_ > 0
            ? static_cast<float>(std::sqrt(m2_ / count_)) : NAN;
    }

private:
    uint32_t count_ {};
    double mean_ {};
    double m2_ {};
};

inline bool validReferenceAltitude(float altitudeM) {
    return std::isfinite(altitudeM) &&
           std::fabs(altitudeM - REFERENCE_ALTITUDE_M) <= 0.05f;
}

inline bool makeSample(float rawPressureHpa, float temperatureC,
                       float seaLevelPressureHpa, Sample& sample) {
    float expectedPressureHpa = NAN;
    if (!std::isfinite(rawPressureHpa) || rawPressureHpa <= 0.0f ||
        !altitude_math::expectedPressureHpa(
            seaLevelPressureHpa, temperatureC,
            REFERENCE_ALTITUDE_M, expectedPressureHpa)) {
        return false;
    }
    const float residual = rawPressureHpa - expectedPressureHpa;
    if (!std::isfinite(residual)) return false;
    sample.residualHpa = residual;
    sample.temperatureC = temperatureC;
    sample.seaLevelPressureHpa = seaLevelPressureHpa;
    return true;
}

inline bool evaluate(Sample* samples, size_t validCount,
                     uint32_t totalSamples, Result& result) {
    result = {};
    result.totalSamples = totalSamples;
    result.validSamples = static_cast<uint32_t>(validCount);
    if (samples == nullptr || totalSamples == 0 ||
        validCount * 100u <
            static_cast<size_t>(totalSamples) * 80u) {
        result.failure = Failure::InsufficientSamples;
        return false;
    }

    std::sort(samples, samples + validCount,
        [](const Sample& left, const Sample& right) {
            return left.residualHpa < right.residualHpa;
        });
    const size_t trim = validCount / 20u;
    const size_t begin = trim;
    const size_t end = validCount - trim;
    if (begin >= end) {
        result.failure = Failure::InvalidStatistics;
        return false;
    }

    RunningStats residualStats;
    for (size_t i = begin; i < end; ++i) {
        residualStats.add(samples[i].residualHpa);
    }
    const float candidateOffset = residualStats.mean();
    result.pressureOffsetHpa = candidateOffset;
    result.usedSamples = residualStats.count();
    if (!std::isfinite(candidateOffset)) {
        result.failure = Failure::InvalidStatistics;
        return false;
    }
    if (std::fabs(candidateOffset) > MAX_OFFSET_HPA) {
        result.failure = Failure::OffsetOutOfRange;
        return false;
    }

    RunningStats temperatureStats;
    RunningStats pressureFieldStats;
    RunningStats rawPressureStats;
    RunningStats expectedPressureStats;
    RunningStats preAltitudeStats;
    RunningStats postAltitudeStats;
    for (size_t i = begin; i < end; ++i) {
        float expectedPressureHpa = NAN;
        if (!altitude_math::expectedPressureHpa(
                samples[i].seaLevelPressureHpa,
                samples[i].temperatureC,
                REFERENCE_ALTITUDE_M, expectedPressureHpa)) {
            result.failure = Failure::InvalidStatistics;
            return false;
        }
        const float rawPressureHpa =
            expectedPressureHpa + samples[i].residualHpa;
        float preAltitudeM = NAN;
        float postAltitudeM = NAN;
        if (!altitude_math::absoluteAltitudeM(
                rawPressureHpa, 0.0f,
                samples[i].seaLevelPressureHpa,
                samples[i].temperatureC, preAltitudeM) ||
            !altitude_math::absoluteAltitudeM(
                rawPressureHpa, candidateOffset,
                samples[i].seaLevelPressureHpa,
                samples[i].temperatureC, postAltitudeM)) {
            result.failure = Failure::InvalidStatistics;
            return false;
        }
        temperatureStats.add(samples[i].temperatureC);
        pressureFieldStats.add(samples[i].seaLevelPressureHpa);
        rawPressureStats.add(rawPressureHpa);
        expectedPressureStats.add(expectedPressureHpa);
        preAltitudeStats.add(preAltitudeM);
        postAltitudeStats.add(postAltitudeM);
    }

    result.meanTemperatureC = temperatureStats.mean();
    result.meanSeaLevelPressureHpa = pressureFieldStats.mean();
    result.meanRawPressureHpa = rawPressureStats.mean();
    result.meanExpectedPressureHpa = expectedPressureStats.mean();
    result.preCalibrationMeanAltitudeM = preAltitudeStats.mean();
    result.preCalibrationStdDevM = preAltitudeStats.populationStdDev();
    result.postCalibrationMeanAltitudeM = postAltitudeStats.mean();
    result.postCalibrationStdDevM = postAltitudeStats.populationStdDev();
    if (!std::isfinite(result.postCalibrationMeanAltitudeM) ||
        std::fabs(result.postCalibrationMeanAltitudeM -
                  REFERENCE_ALTITUDE_M) > MAX_MEAN_ERROR_M) {
        result.failure = Failure::MeanAltitudeOutOfRange;
        return false;
    }
    if (!std::isfinite(result.postCalibrationStdDevM) ||
        result.postCalibrationStdDevM > MAX_STDDEV_M) {
        result.failure = Failure::AltitudeNoiseTooHigh;
        return false;
    }
    result.success = true;
    result.failure = Failure::None;
    return true;
}

} // namespace bmp581_calibration
} // namespace utils
