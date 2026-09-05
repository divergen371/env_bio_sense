#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <MAX30105.h>
#include "utils/pulse_analyzer.h"
#include "core/ppg_sample_sink.h"

namespace drivers {
namespace sensors {

class Max30102Sensor : public ISensor {
public:
    Max30102Sensor();

    // ISensor 実装
    core::SensorId id() const override { return core::SensorId::Max30102; }
    bool begin() override;
    void update(uint32_t nowMs) override;
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }

    // 生データおよびHR/SpO2データの取得 (Step 6では生データのみ格納)
    bool readPpg(core::PpgData& out) const;
    void setSampleSink(core::IPpgSampleSink* sink) { sampleSink_ = sink; }
    uint32_t droppedSamples() const { return droppedSamples_; }
    uint32_t fifoOverflows() const { return fifoOverflows_; }

private:
    MAX30105 particleSensor_;
    
    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    
    core::PpgData currentData_;
    bool hasValidData_ {false};
    core::IPpgSampleSink* sampleSink_ {nullptr};
    uint32_t logicalSampleIndex_ {0};
    uint32_t droppedSamples_ {0};
    uint32_t fifoOverflows_ {0};
    uint32_t lastOverflowPollMs_ {0};
    uint32_t lastReinitAttemptMs_ {0};

    // --- Step 9: Custom DSP Analyzer ---
    utils::PulseAnalyzer analyzer_;

    // --- Step 8: Calibration Variables ---
    core::PpgState ppgState_ {core::PpgState::NoFinger};
    uint8_t currentLedBrightness_ {0};
    uint32_t calibStartMs_ {0};

    void resetPpgState();
};

} // namespace sensors
} // namespace drivers
