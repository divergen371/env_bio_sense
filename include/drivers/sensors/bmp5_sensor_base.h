#pragma once

#include "drivers/sensors/sensor_interface.h"
#include "utils/bmp581_calibration.h"
#include "bmp5.h"
#include <freertos/FreeRTOS.h>

namespace drivers {
namespace sensors {

class Bmp5SensorBase : public IEnvironmentSensor {
public:
    Bmp5SensorBase();
    virtual ~Bmp5SensorBase() = default;

    bool begin() override;
    void update(uint32_t nowMs) override;
    bool readEnvironment(core::EnvironmentData& out) const override;
    
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }
    uint32_t consecutiveErrors() const { return consecutiveErrors_; }
    
    // 海面気圧の設定（高度計算用）
    void setSeaLevelPressure(float hpa,
        core::PressureFieldState state = core::PressureFieldState::Valid);
    void setPressureFieldState(core::PressureFieldState state);
    float getSeaLevelPressure() const;
    core::PressureFieldState getPressureFieldState() const;
    
    // 校正
    void setCalibrationOffset(float offsetHpa);
    float getCalibrationOffset() const;
    float getRawAltitude() const;
    float getDisplayAltitude() const;
    bool getAltitudeTemperature(float& temperatureC,
                                bool& externalSource) const;
    bool startCalibration(float referenceAltitudeM = 13.6f);
    void cancelCalibration();
    bool isCalibrating() const;
    bool takeCalibrationResult(utils::bmp581_calibration::Result& result);
    void processCalibration(uint32_t nowMs, float pressureHpa, float tempC,
                            bool externalTemperatureSource = false);
    
    // 外部基準気温の設定（高度計算用。NANで無効化）
    void setReferenceTemperature(float tempC) { referenceTemperatureC_ = tempC; }
    
    // 診断用
    void runDiagnostics();

protected:
    // 派生クラスで実装する固有パラメータ
    virtual uint8_t getI2cAddress() const = 0;
    virtual uint8_t getExpectedChipId() const = 0;
    virtual const char* getSensorName() const = 0;

private:
    static constexpr uint32_t DATA_MAX_AGE_MS = 3000;

    struct bmp5_dev bmp5_dev_;
    struct bmp5_osr_odr_press_config osr_odr_press_cfg_;

    uint8_t dev_addr_;
    uint32_t lastSuccessMs_ = 0;
    float currentPressureHpa_ = 0.0f;
    float currentTemperatureC_ = 0.0f;
    float rawAbsoluteAltitudeM_ = NAN;
    float displayAltitudeM_ = NAN; // ヒステリシス適用後の高度
    bool altitudeValid_ = false;
    
    float interpolatedSeaLevelPressureHpa_ = 1013.25f; // Fallback
    core::PressureFieldState pressureFieldState_ = core::PressureFieldState::Invalid;
    float pressureOffsetHpa_ = 0.0f;
    mutable portMUX_TYPE dataMux_ = portMUX_INITIALIZER_UNLOCKED;
    
    float referenceTemperatureC_ = NAN; // SHT45等からの外部気温
    float altitudeTemperatureC_ = NAN;
    bool altitudeTemperatureExternal_ = false;
    bool isStale_ = true;
    bool hasValidData_ = false;

    core::DeviceState state_ = core::DeviceState::Unknown;
    core::ErrorCode lastError_ = core::ErrorCode::None;

    uint32_t consecutiveErrors_ = 0;
    uint32_t resetCount_ = 0;
    uint32_t nextReinitMs_ = 0;

    // 校正用
    bool isCalibrating_ = false;
    uint32_t calibStateStartMs_ = 0;
    uint32_t calibLastSampleMs_ = 0;
    uint8_t calibPhase_ = 0; // 0: Idle, 1: Settle, 2: Collect
    uint32_t calibValidSamples_ = 0;
    uint32_t calibTotalSamples_ = 0;
    uint32_t calibExternalTemperatureSamples_ = 0;
    utils::bmp581_calibration::Sample
        calibSamples_[utils::bmp581_calibration::EXPECTED_SAMPLES] {};
    utils::bmp581_calibration::Result calibResult_ {};
    bool calibResultReady_ = false;

    void finishCalibration(
        utils::bmp581_calibration::Failure forcedFailure =
            utils::bmp581_calibration::Failure::None);

    bool reinitSensor(uint32_t nowMs);
    void changeState(core::DeviceState newState);

    static int8_t i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr);
    static int8_t i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr);
    static void delay_us(uint32_t period, void *intf_ptr);
};

} // namespace sensors
} // namespace drivers
