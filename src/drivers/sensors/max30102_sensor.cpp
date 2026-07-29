#include "drivers/sensors/max30102_sensor.h"
#include "services/logger.h"
#include <Wire.h>
#include <spo2_algorithm.h>

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
                bufferIndex_ = 0;
            }
        } else if (ppgState_ == core::PpgState::NoFinger) {
            // 指が検出された
            ppgState_ = core::PpgState::Calibrating;
            currentLedBrightness_ = 10;
            particleSensor_.setPulseAmplitudeRed(currentLedBrightness_);
            particleSensor_.setPulseAmplitudeIR(currentLedBrightness_);
            calibStartMs_ = nowMs;
            bufferIndex_ = 0;
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
                bufferIndex_ = 0;
            }

            // タイムアウトフォールバック (5秒)
            if (nowMs - calibStartMs_ > 5000) {
                ppgState_ = core::PpgState::Measuring;
                bufferIndex_ = 0;
            }
        }

        // 3. 測定中の処理
        if (ppgState_ == core::PpgState::Measuring) {
            if (bufferIndex_ < BUFFER_LENGTH) {
                redBuffer_[bufferIndex_] = red;
                irBuffer_[bufferIndex_] = ir;
                bufferIndex_++;
            }

            if (bufferIndex_ == BUFFER_LENGTH) {
                // AC振幅（波形の大きさ）をチェックして押し付け圧を評価
                uint32_t minIR = 0xFFFFFFFF;
                uint32_t maxIR = 0;
                for (int i = 0; i < BUFFER_LENGTH; i++) {
                    if (irBuffer_[i] < minIR) minIR = irBuffer_[i];
                    if (irBuffer_[i] > maxIR) maxIR = irBuffer_[i];
                }
                uint32_t amplitude = maxIR - minIR;
                currentData_.signalAmplitude = amplitude;
                
                // 振幅が小さすぎる(強すぎ/弱すぎ) または 大きすぎる(ノイズ) 場合は警告
                if (amplitude < 1000 || amplitude > 15000) {
                    currentData_.signalPoor = true;
                } else {
                    currentData_.signalPoor = false;
                }

                maxim_heart_rate_and_oxygen_saturation(
                    irBuffer_, BUFFER_LENGTH, redBuffer_,
                    &lastSpo2_, &spo2Valid_,
                    &lastHeartRate_, &hrValid_
                );

                const int shiftAmount = 25;
                for (int i = shiftAmount; i < BUFFER_LENGTH; i++) {
                    redBuffer_[i - shiftAmount] = redBuffer_[i];
                    irBuffer_[i - shiftAmount] = irBuffer_[i];
                }
                bufferIndex_ = BUFFER_LENGTH - shiftAmount;

                // 外れ値カットとEMAフィルタ
                if (hrValid_ && spo2Valid_ && lastSpo2_ > 0 && lastHeartRate_ > 0) {
                    if (lastHeartRate_ >= 40 && lastHeartRate_ <= 200) {
                        float newHr = static_cast<float>(lastHeartRate_);
                        float newSpo2 = static_cast<float>(lastSpo2_);
                        
                        if (!currentData_.calculatedValid) {
                            smoothedHr_ = newHr;
                            smoothedSpo2_ = newSpo2;
                        } else {
                            smoothedHr_ = (0.2f * newHr) + (0.8f * smoothedHr_);
                            smoothedSpo2_ = (0.2f * newSpo2) + (0.8f * smoothedSpo2_);
                        }
                        
                        currentData_.heartRateBpm = smoothedHr_;
                        currentData_.spo2Percent = smoothedSpo2_;
                        currentData_.calculatedValid = true;
                    }
                }
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
    bufferIndex_ = 0;
    currentData_.calculatedValid = false;
    currentData_.signalPoor = false;
    currentData_.heartRateBpm = 0;
    currentData_.spo2Percent = 0;
    currentData_.state = ppgState_;
}

} // namespace sensors
} // namespace drivers
