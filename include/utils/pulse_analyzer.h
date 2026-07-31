#pragma once

#include <cstdint>
#include <algorithm>

namespace utils {

class PulseAnalyzer {
public:
    PulseAnalyzer();
    
    // 1サンプルの処理。新しいビート（心拍）を検出したら true を返す
    bool processSample(uint32_t irRaw, uint32_t redRaw, uint32_t timestampMs);
    
    // 直近に計算されたBPM
    float getHeartRateBpm() const { return heartRate_; }
    // 直近に計算されたSpO2 (%)
    float getSpo2Percent() const { return spo2_; }
    // リアルタイム信号振幅（常時更新される）
    float getSignalAmplitude() const { return displayAmplitude_; }
    
    // アルゴリズムの状態をリセット（指が離れた時などに呼ぶ）
    void reset();

private:
    // DC除去フィルタ
    float irDc_{0.0f};
    float redDc_{0.0f};
    
    // AC成分（フィルタ済み）
    float irAc_{0.0f};
    float redAc_{0.0f};
    float irAcPrev_{0.0f};
    
    // ピーク検出用 Min/Max（ゼロクロスでリセットされる）
    float irMax_{0.0f};
    float irMin_{0.0f};
    
    // SpO2計算用 Min/Max（ビート検出時にのみリセット）
    float redMin_{0.0f};
    float redMax_{0.0f};
    
    // リアルタイム振幅表示用（スライディングウィンドウ）
    float displayAmpMax_{0.0f};
    float displayAmpMin_{0.0f};
    float displayAmplitude_{0.0f};
    
    uint32_t lastPeakTimeMs_{0};
    uint32_t lastBeatTimeMs_{0};
    uint32_t lastInterval_{1000};
    
    // DC高速初期化用のサンプルカウンタ
    uint32_t sampleCount_{0};
    
    float heartRate_{0.0f};
    float spo2_{0.0f};
    
    bool waitingForPeak_{true};
};

} // namespace utils
