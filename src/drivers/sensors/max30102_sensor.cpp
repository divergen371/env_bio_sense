#include "drivers/sensors/max30102_sensor.h"
#include "services/logger.h"
#include "hal/i2c_bus.h"
#include <Wire.h>

namespace drivers {
namespace sensors {

Max30102Sensor::Max30102Sensor() {}

bool Max30102Sensor::begin() {
    services::Logger::info("MAX30102", "Initializing MAX30102...");
    state_ = core::DeviceState::Initializing;
    
    hal::I2cLockGuard lock(100);
    if (!lock.acquired()) {
        services::Logger::error("MAX30102", "Failed to acquire I2C lock during begin().");
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

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
    byte sampleAverage = 4;  // 4サンプル移動平均（ノイズキャンセル強化）
    byte ledMode = 2;        // Red + IR モード
    byte sampleRate = 400;   // 400Hz設定（sampleAverage=4 により実効 100Hz = 10ms間隔）
    int pulseWidth = 411;    // 411μs (18-bit 最高分解能)
    int adcRange = 4096;     // 4096 nA フルスケールレンジ
    
    particleSensor_.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
    resetPpgState();

    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    services::Logger::info("MAX30102", "MAX30102 configured and ready (Effective 100Hz).");
    return true;
}

void Max30102Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }

    hal::I2cLockGuard lock(10); // PPGは高速サンプリングなのでタイムアウトは短く設定 (10ms)
    if (!lock.acquired()) {
        // OLED描画などでI2Cがロック中の場合はスキップして次回に回す
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
                currentLedBrightness_ += 5; // ステップ幅を+5に広げて高速調整
                if (currentLedBrightness_ > 250) currentLedBrightness_ = 250;
                adjusted = true;
            } else if (ir > 120000 && currentLedBrightness_ > 10) {
                currentLedBrightness_ -= 5; // ステップ幅を-5に広げて高速調整
                if (currentLedBrightness_ < 10) currentLedBrightness_ = 10;
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
            // 加えて、PIが20%を超えるような異常な大振幅も体動・ノイズとみなして警告
            float pi = analyzer_.getPerfusionIndex();
            if (amplitude < 50.0f || amplitude > 30000.0f || pi > 20.0f) {
                currentData_.signalPoor = true;
            } else {
                currentData_.signalPoor = false;
            }

            float hr = analyzer_.getHeartRateBpm();
            float spo2 = analyzer_.getSpo2Percent();
            float dptHr = analyzer_.getDptHeartRateBpm();
            float dptSpo2 = analyzer_.getDptSpo2Percent();
            
            currentData_.dptHeartRateBpm = dptHr;
            currentData_.dptSpo2Percent = dptSpo2;
            currentData_.perfusionIndex = analyzer_.getPerfusionIndex();
            
            // ピーク検出が有効な場合のみ数値を画面に送る。
            // 以前はピーク検出ロスト時にDPTへフォールバックしていたが、
            // 体動（指の揺れ）による巨大なノイズ周波数をDPTが拾ってしまい
            // 画面の数値が暴れる原因となっていたため、フォールバックを廃止する。
            if (hr > 0.0f && spo2 > 0.0f) {
                currentData_.heartRateBpm = hr;
                currentData_.spo2Percent = spo2;
                currentData_.calculatedValid = true;
            } else {
                // ピーク検出が途絶えた（タイムアウトした）場合は計算中(ロスト)扱いとする
                // ただし物理的なシグナル強度(amplitude/pi)が正常なら signalPoor は上書きしない
                // これにより、UI上は「Measuring (Calc...)」となる
                currentData_.heartRateBpm = 0.0f;
                currentData_.spo2Percent = 0.0f;
                currentData_.calculatedValid = false;
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
    
    // I2C通信が発生するため、呼び出し元でロックを取得している前提
    particleSensor_.setPulseAmplitudeRed(0);
    particleSensor_.setPulseAmplitudeIR(10);
    analyzer_.reset();
    currentData_.calculatedValid = false;
    currentData_.signalPoor = false;
    currentData_.heartRateBpm = 0;
    currentData_.spo2Percent = 0;
    currentData_.dptHeartRateBpm = 0;
    currentData_.dptSpo2Percent = 0;
    currentData_.perfusionIndex = 0;
    currentData_.state = ppgState_;
}

} // namespace sensors
} // namespace drivers
