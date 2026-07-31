#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

namespace utils {

// DPT解析対象の周期範囲（100Hzサンプリング時）
// 30 BPM = 2.0秒 = 200サンプル周期
// 240 BPM = 0.25秒 = 25サンプル周期
static constexpr int DPT_PERIOD_MIN = 25;   // 240 BPM
static constexpr int DPT_PERIOD_MAX = 133;  // 45 BPM
static constexpr int DPT_NUM_PERIODS = DPT_PERIOD_MAX - DPT_PERIOD_MIN + 1; // 109ビン
static constexpr int DPT_BUFFER_SIZE = 512;  // 5秒 @100Hz（ESP32S3メモリ制約に配慮）

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
    // 灌流指数 PI (%) - 信号品質の指標。0.2%未満は低灌流
    float getPerfusionIndex() const { return perfusionIndex_; }
    // DPTによるBPM推定値（周波数領域解析）
    float getDptHeartRateBpm() const { return dptHeartRate_; }
    // DPTによるSpO2推定値（周波数領域解析）
    float getDptSpo2Percent() const { return dptSpo2_; }
    
    // アルゴリズムの状態をリセット（指が離れた時などに呼ぶ）
    void reset();

private:
    // ===== Step A: 時間領域フィルタ・ピーク検出 =====
    
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
    float perfusionIndex_{0.0f};
    
    bool waitingForPeak_{true};
    
    // ===== Step B: スライディングDPT (Sliding Discrete Period Transform) =====
    
    // 循環バッファ（IR / Red のAC成分を保存）
    // 512サンプル = 約5秒 @100Hz（ESP32S3のメモリ制約に配慮）
    float irAcBuffer_[DPT_BUFFER_SIZE]{};
    float redAcBuffer_[DPT_BUFFER_SIZE]{};
    int bufferIdx_{0};
    int bufferFilled_{0};  // バッファに何サンプル入ったか（最大DPT_BUFFER_SIZE）
    
    // DPTスペクトル（各周期ビンごとのパワー、analyzeDptSpectrum内でローカル計算）
    // → ヒープ節約のため結果のみ保持
    
    // DPT解析結果
    float dptHeartRate_{0.0f};
    float dptSpo2_{0.0f};
    
    // DPT解析の実行間隔管理
    uint32_t lastDptTimeMs_{0};
    
    // DPTスペクトルの更新と解析
    void updateDpt(float irAcSample, float redAcSample);
    void analyzeDptSpectrum();
};

} // namespace utils
