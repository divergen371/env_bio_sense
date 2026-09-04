#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <SensirionI2CSht4x.h>
#include <atomic>

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
    uint32_t consecutiveErrors() const { return consecutiveErrors_; }

    // IEnvironmentSensor 実装
    bool readEnvironment(core::EnvironmentData& out) const override;

private:
    enum class HeaterPhase : uint8_t { Idle, Waiting, Cooldown };

    static constexpr uint8_t HEATER_ACTIVE = 0x80;
    static constexpr uint32_t DATA_MAX_AGE_MS = 3000;
    static constexpr uint32_t RETRY_DELAY_MS = 60000;
    static constexpr uint32_t HEATER_COOLDOWN_MS = 15000;

    static uint8_t heaterCommand(HeaterPower power, HeaterDuration duration);
    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs);
    void processHeater(uint32_t nowMs);
    bool sendHeaterCommand(uint8_t command);
    bool readHeaterResult(float& temperatureC, float& humidityRh);
    void markFailure(core::ErrorCode error, uint32_t nowMs,
                     const char* operation);

    SensirionI2cSht4x sht4x_;
    core::DeviceState state_ = core::DeviceState::Offline;
    core::ErrorCode lastError_ = core::ErrorCode::None;
    
    float currentTemperature_ = 0.0f;
    float currentHumidity_ = 0.0f;
    bool hasValidData_ = false;

    std::atomic<uint8_t> heaterRequest_ {0};
    HeaterPhase heaterPhase_ {HeaterPhase::Idle};
    uint32_t heaterDeadlineMs_ {0};
    uint32_t retryAtMs_ {0};

    // 統計・診断用
    uint32_t successCount_ = 0;
    uint32_t readErrorCount_ = 0;
    uint32_t notReadyCount_ = 0;
    uint32_t consecutiveErrors_ = 0;
    uint32_t lastSuccessMs_ = 0;
};

} // namespace sensors
} // namespace drivers
