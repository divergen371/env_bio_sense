#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <SensirionI2CSht4x.h>

namespace drivers {
namespace sensors {

class Sht45Sensor : public IEnvironmentSensor {
public:
    Sht45Sensor();
    
    // ISensor 実装
    core::SensorId id() const override { return core::SensorId::Sht45; }
    bool begin() override;
    
    // ヒーター起動（結露防止・復旧用）
    // 引数で強度（Highest, Medium, Lowest）と持続時間（Long=1s, Short=0.1s）を指定可能とする簡易ラッパー
    enum class HeaterPower { Highest, Medium, Lowest };
    enum class HeaterDuration { Long, Short };
    bool triggerHeater(HeaterPower power = HeaterPower::Highest, HeaterDuration duration = HeaterDuration::Long);

    void update(uint32_t nowMs) override;
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }

    // IEnvironmentSensor 実装
    bool readEnvironment(core::EnvironmentData& out) const override;

private:
    SensirionI2cSht4x sht4x_;
    core::DeviceState state_ = core::DeviceState::Offline;
    core::ErrorCode lastError_ = core::ErrorCode::None;
    
    float currentTemperature_ = 0.0f;
    float currentHumidity_ = 0.0f;
    bool hasValidData_ = false;

    uint32_t lastReadMs_ = 0;
    uint32_t heaterCooldownUntilMs_ = 0;
    float preHeaterTempC_ = 0.0f;
    float preHeaterHumRh_ = 0.0f;

    // 統計・診断用
    uint32_t successCount_ = 0;
    uint32_t readErrorCount_ = 0;
    uint32_t notReadyCount_ = 0;
    uint32_t consecutiveErrors_ = 0;
    uint32_t lastSuccessMs_ = 0;
};

} // namespace sensors
} // namespace drivers
