#pragma once

#include <cstdint>

namespace utils {
namespace scd41_maintenance {

struct FrcSequenceResult {
    uint16_t pressureError {0};
    uint16_t stopError {0};
    uint16_t frcError {0};
    uint16_t restartError {0};
    uint16_t rawWord {0};
    bool frcAttempted {false};
    bool restartAttempted {false};
};

// Each callback owns one short I2C transaction. The wait callback executes
// without holding the shared bus lock. Once the sensor is stopped and FRC is
// attempted, restart is always attempted even if FRC itself fails.
template <typename ApplyPressure, typename Stop, typename Wait,
          typename PerformFrc, typename Restart>
FrcSequenceResult runFrcSequence(uint16_t referencePpm,
                                 ApplyPressure applyPressure,
                                 Stop stop,
                                 Wait waitAfterStop,
                                 PerformFrc performFrc,
                                 Restart restart) {
    FrcSequenceResult result;
    result.pressureError = applyPressure();
    if (result.pressureError != 0) return result;

    result.stopError = stop();
    if (result.stopError != 0) return result;

    waitAfterStop();
    result.frcAttempted = true;
    result.frcError = performFrc(referencePpm, result.rawWord);
    result.restartAttempted = true;
    result.restartError = restart();
    return result;
}

} // namespace scd41_maintenance
} // namespace utils
