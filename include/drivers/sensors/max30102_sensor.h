#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <MAX30105.h>

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

private:
    MAX30105 particleSensor_;
    
    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    
    core::PpgData currentData_;
    bool hasValidData_ {false};

    // --- Step 7: HR/SpO2 Algorithm Variables ---
    static constexpr int BUFFER_LENGTH = 100;
    uint32_t irBuffer_[BUFFER_LENGTH] = {0};
    uint32_t redBuffer_[BUFFER_LENGTH] = {0};
    int bufferIndex_ = 0;
    
    // 計算結果保持
    int32_t lastSpo2_ = 0;
    int8_t spo2Valid_ = 0;
    int32_t lastHeartRate_ = 0;
    int8_t hrValid_ = 0;

    // --- Step 8: Calibration & EMA Filter Variables ---
    core::PpgState ppgState_ {core::PpgState::NoFinger};
    uint8_t currentLedBrightness_ {0};
    uint32_t calibStartMs_ {0};

    float smoothedHr_ {0.0f};
    float smoothedSpo2_ {0.0f};

    void resetPpgState();
};

} // namespace sensors
} // namespace drivers
