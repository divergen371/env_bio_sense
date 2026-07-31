#include "utils/pulse_analyzer.h"
#include <cmath>
#include <algorithm>

namespace utils {

PulseAnalyzer::PulseAnalyzer() {
    reset();
}

void PulseAnalyzer::reset() {
    irDc_ = 0.0f;
    redDc_ = 0.0f;
    irAc_ = 0.0f;
    redAc_ = 0.0f;
    irAcPrev_ = 0.0f;
    irMin_ = 0.0f;
    irMax_ = 0.0f;
    redMin_ = 0.0f;
    redMax_ = 0.0f;
    displayAmpMax_ = 0.0f;
    displayAmpMin_ = 0.0f;
    displayAmplitude_ = 0.0f;
    lastPeakTimeMs_ = 0;
    lastBeatTimeMs_ = 0;
    lastInterval_ = 1000;
    sampleCount_ = 0;
    heartRate_ = 0.0f;
    spo2_ = 0.0f;
    waitingForPeak_ = true;
}

bool PulseAnalyzer::processSample(uint32_t irRaw, uint32_t redRaw, uint32_t timestampMs) {
    // 1. 初回初期化
    if (irDc_ == 0.0f || irRaw == 0 || redRaw == 0) {
        irDc_ = (float)irRaw;
        redDc_ = (float)redRaw;
        return false;
    }
    
    sampleCount_++;
    
    // 2. DC成分のトラッキング
    // 最初の200サンプル(約2秒)は高速α=0.1で急速収束。以降はα=0.01でゆっくり追従。
    // これにより指を置いてから2秒以内にDCが安定し、AC成分の抽出が正確になる。
    float dcAlpha = (sampleCount_ < 200) ? 0.1f : 0.01f;
    irDc_ = irDc_ * (1.0f - dcAlpha) + (float)irRaw * dcAlpha;
    redDc_ = redDc_ * (1.0f - dcAlpha) + (float)redRaw * dcAlpha;
    
    // 3. AC成分の抽出
    // MAX30102は反射型。心拍（血流増）で光吸収が増え値は「下がる」。
    // マイナスを掛けて位相を反転し、ピークを「山」として扱う。
    float irAcRaw = -((float)irRaw - irDc_);
    float redAcRaw = -((float)redRaw - redDc_);
    
    // ローパスフィルタ (α=0.15) で高周波ノイズを除去
    irAc_ = irAc_ * 0.85f + irAcRaw * 0.15f;
    redAc_ = redAc_ * 0.85f + redAcRaw * 0.15f;
    
    // 4. リアルタイム振幅の表示用トラッキング（ビート検知とは完全に独立）
    // irAc_の瞬時Max/Minを追い、ゆっくり減衰させる。常に「今の波の大きさ」を反映する。
    if (irAc_ > displayAmpMax_) displayAmpMax_ = irAc_;
    if (irAc_ < displayAmpMin_) displayAmpMin_ = irAc_;
    // 約0.5秒(25サンプル)でMax/Minが50%減衰: 0.97^25 ≈ 0.47
    displayAmpMax_ *= 0.97f;
    displayAmpMin_ *= 0.97f;
    displayAmplitude_ = displayAmpMax_ - displayAmpMin_;
    
    // 5. ピーク検出用 Min/Max トラッキング
    if (irAc_ > irMax_) irMax_ = irAc_;
    if (irAc_ < irMin_) irMin_ = irAc_;
    if (redAc_ > redMax_) redMax_ = redAc_;
    if (redAc_ < redMin_) redMin_ = redAc_;
    
    // 6. ゼロクロス再武装とピーク検出ロジック
    bool beatDetected = false;
    
    // 0を下回った瞬間（ゼロクロス・ダウン）で次のピーク探索を再武装
    if (irAc_ < 0.0f && irAcPrev_ >= 0.0f) {
        waitingForPeak_ = true;
        irMax_ = 0.0f; // ピーク検出用のMaxをリセット
        // irMin_はリセットしない（SpO2計算で必要）
        // redMin_/redMax_もリセットしない
    }
    
    // 不応期: 固定250ms (最大240BPM)
    uint32_t currentRefractory = 250;
    
    // 山の高さが最低限(10)以上ある場合のみピーク判定
    // ※ローパスフィルタ(α=0.15)は25Hz実効サンプリングレートで信号を約54%に減衰させるため
    //   閾値は低めに設定する必要がある
    if (waitingForPeak_ && irMax_ > 10.0f) {
        // 山の高さの30%降下、最低5ユニットでピーク確定
        float dropThreshold = std::max(5.0f, irMax_ * 0.3f);
        
        if (timestampMs - lastPeakTimeMs_ > currentRefractory) {
            // ピークは必ずプラス領域、かつ頂上から十分下がった地点で確定
            if (irAc_ > 0.0f && irAc_ < irMax_ - dropThreshold) {
                beatDetected = true;
                waitingForPeak_ = false;
                
                // --- 心拍数(HR)の計算 ---
                if (lastBeatTimeMs_ != 0) {
                    uint32_t interval = timestampMs - lastBeatTimeMs_;
                    
                    if (interval >= 270 && interval <= 2000) {
                        lastInterval_ = interval;
                        float instantBpm = 60000.0f / (float)interval;
                        
                        if (heartRate_ == 0.0f) {
                            heartRate_ = instantBpm;
                        } else {
                            heartRate_ = heartRate_ * 0.8f + instantBpm * 0.2f;
                        }
                    }
                }
                lastBeatTimeMs_ = timestampMs;
                lastPeakTimeMs_ = timestampMs;
                
                // --- SpO2の計算 (Ratio-of-Ratios) ---
                // Maxim MAXREFDES117# リファレンスデザインの公式キャリブレーションを使用
                // MAX30102 (Red: 660nm / IR: 880nm) に最適化された二次多項式
                float irAmplitude = irMax_ - irMin_;
                float redAmplitude = redMax_ - redMin_;
                
                // SpO2の精度を確保するための条件:
                // 1. DC高速初期化が完了していること (最初の200サンプルはDCが不安定)
                // 2. IR/Red両方の振幅が最低限あること (小振幅ではR比がノイズに支配される)
                // 3. DC成分が正の値であること
                if (sampleCount_ > 200 &&
                    irAmplitude > 20.0f && redAmplitude > 20.0f &&
                    irDc_ > 0 && redDc_ > 0) {
                    
                    // R = (RedAC/RedDC) / (IrAC/IrDC)
                    float r = (redAmplitude / redDc_) / (irAmplitude / irDc_);
                    
                    // R比が妥当な範囲(0.02〜1.84)のときだけ更新
                    // ※Maxim公式テーブルの有効範囲に合わせる
                    if (r >= 0.02f && r <= 1.84f) {
                        // Maxim公式: SpO2 = -45.060*R^2 + 30.354*R + 94.845
                        // (spo2_algorithm.h 72行目より引用)
                        float instantSpo2 = -45.060f * r * r + 30.354f * r + 94.845f;
                        instantSpo2 = std::max(0.0f, std::min(100.0f, instantSpo2));
                        
                        if (spo2_ == 0.0f) {
                            spo2_ = instantSpo2;
                        } else {
                            // 重い平滑化 (0.95/0.05) でSpO2を安定させる
                            spo2_ = spo2_ * 0.95f + instantSpo2 * 0.05f;
                        }
                    }
                }
                
                // 次のビートに向けてMin/Maxを現在値でリセット
                irMin_ = irAc_;
                irMax_ = irAc_;
                redMin_ = redAc_;
                redMax_ = redAc_;
            }
        }
    }
    
    // ピーク検出用Min/Maxの緩やかな減衰
    irMax_ *= 0.999f;
    irMin_ *= 0.999f;
    redMax_ *= 0.999f;
    redMin_ *= 0.999f;
    
    irAcPrev_ = irAc_;
    
    return beatDetected;
}

} // namespace utils
