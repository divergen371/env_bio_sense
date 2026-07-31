#include "drivers/sensors/max30102_sensor.h"
#include "services/logger.h"
#include <Wire.h>

namespace drivers {
namespace sensors {

Max30102Sensor::Max30102Sensor() {}

bool Max30102Sensor::begin() {
    services::Logger::info("MAX30102", "Initializing MAX30102...");
    state_ = core::DeviceState::Initializing;
    
    // Wire インスタンスと I2C speed, I2C address を渡す (デフォルト0x57)
    // begin(wirePort, i2cSpeed, i2caddr)
    // 第2引数の I2C_SPEED_STANDARD = 100kHz, I2C_SPEED_FAST = 400kHz
    if (!particleSensor_.begin(Wire, I2C_SPEED_STANDARD, 0x57)) {
        services::Logger::error("MAX30102", "MAX30102 was not found. Please check wiring/power.");
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }
    
    services::Logger::info("MAX30102", "MAX30102 found. Configuring...");

    // 基本設定 (Sensor setup)
    byte ledBrightness = 10; // 初期値(待機用)
    byte sampleAverage = 4;
    byte ledMode = 2;
    byte sampleRate = 100;
    int pulseWidth = 411;
    int adcRange = 4096;
    
    particleSensor_.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
    resetPpgState();

    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    services::Logger::info("MAX30102", "MAX30102 configured and ready.");
    return true;
}

void Max30102Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }

    // 新しいサンプル（Red / IR）がFIFOバッファにあるかチェック
    particleSensor_.check(); // FIFOの中の未読データをバッファに取り込む

    while (particleSensor_.available()) {
        uint32_t red = particleSensor_.getFIFORed();
        uint32_t ir = particleSensor_.getFIFOIR();

        currentData_.red = red;
        currentData_.ir = ir;
        currentData_.timestampMs = nowMs;
        currentData_.rawValid = true; 

        // 1. 指の接触判定とDCベースラインの監視
        if (ir < 20000) {
            if (ppgState_ != core::PpgState::NoFinger) {
                resetPpgState();
            }
        } else if (ppgState_ == core::PpgState::Measuring) {
            // 測定中にDCベースラインが大きくズレたら再キャリブレーション
            if (ir < 50000 || ir > 150000) {
                ppgState_ = core::PpgState::Calibrating;
                calibStartMs_ = nowMs;
            }
        } else if (ppgState_ == core::PpgState::NoFinger) {
            // 指が検出された
            ppgState_ = core::PpgState::Calibrating;
            currentLedBrightness_ = 10;
            particleSensor_.setPulseAmplitudeRed(currentLedBrightness_);
            particleSensor_.setPulseAmplitudeIR(currentLedBrightness_);
            calibStartMs_ = nowMs;
        }

        // 2. キャリブレーション中の処理
        if (ppgState_ == core::PpgState::Calibrating) {
            bool adjusted = false;
            if (ir < 80000 && currentLedBrightness_ < 250) {
                currentLedBrightness_ += 2;
                adjusted = true;
            } else if (ir > 120000 && currentLedBrightness_ > 10) {
                currentLedBrightness_ -= 2;
                adjusted = true;
            }

            if (adjusted) {
                particleSensor_.setPulseAmplitudeRed(currentLedBrightness_);
                particleSensor_.setPulseAmplitudeIR(currentLedBrightness_);
            } else {
                // 適正範囲に入った、または限界
                ppgState_ = core::PpgState::Measuring;
            }

            // タイムアウトフォールバック (5秒)
            if (nowMs - calibStartMs_ > 5000) {
                ppgState_ = core::PpgState::Measuring;
            }
        }

        // 3. 測定中の処理
        if (ppgState_ == core::PpgState::Measuring) {
            bool newBeat = analyzer_.processSample(ir, red, nowMs);
            
            // AC振幅（波形の大きさ）をチェックして押し付け圧を評価
            float amplitude = analyzer_.getSignalAmplitude();
            currentData_.signalAmplitude = (uint32_t)amplitude;
            
            // 閾値を大幅に緩和（500未満、または30000超え のみ警告）
            if (amplitude < 50.0f || amplitude > 30000.0f) {
                currentData_.signalPoor = true;
            } else {
                currentData_.signalPoor = false;
            }

            float hr = analyzer_.getHeartRateBpm();
            float spo2 = analyzer_.getSpo2Percent();
            
            if (hr > 0.0f && spo2 > 0.0f) {
                currentData_.heartRateBpm = hr;
                currentData_.spo2Percent = spo2;
                currentData_.calculatedValid = true;
            }
        }

        currentData_.state = ppgState_;

        particleSensor_.nextSample();
        hasValidData_ = true;
        lastSuccessMs_ = nowMs;
    }
}

bool Max30102Sensor::readPpg(core::PpgData& out) const {
    if (!hasValidData_) return false;
    out = currentData_;
    return true;
}

void Max30102Sensor::resetPpgState() {
    ppgState_ = core::PpgState::NoFinger;
    currentLedBrightness_ = 10;
    particleSensor_.setPulseAmplitudeRed(0);
    particleSensor_.setPulseAmplitudeIR(10);
    analyzer_.reset();
    currentData_.calculatedValid = false;
    currentData_.signalPoor = false;
    currentData_.heartRateBpm = 0;
    currentData_.spo2Percent = 0;
    currentData_.state = ppgState_;
}

} // namespace sensors
} // namespace drivers
