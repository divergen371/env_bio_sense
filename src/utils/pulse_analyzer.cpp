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
    
    // DPT 初期化
    bufferIdx_ = 0;
    bufferFilled_ = 0;
    dptHeartRate_ = 0.0f;
    dptSpo2_ = 0.0f;
    lastDptTimeMs_ = 0;
    for (int i = 0; i < DPT_BUFFER_SIZE; i++) {
        irAcBuffer_[i] = 0.0f;
        redAcBuffer_[i] = 0.0f;
    }
}

bool PulseAnalyzer::processSample(uint32_t irRaw, uint32_t redRaw, uint32_t timestampMs) {
    if (irDc_ == 0.0f || irRaw == 0 || redRaw == 0) {
        irDc_ = (float)irRaw;
        redDc_ = (float)redRaw;
        return false;
    }
    
    sampleCount_++;
    
    float dcAlpha = (sampleCount_ < 200) ? 0.1f : 0.01f;
    irDc_ = irDc_ * (1.0f - dcAlpha) + (float)irRaw * dcAlpha;
    redDc_ = redDc_ * (1.0f - dcAlpha) + (float)redRaw * dcAlpha;
    
    float irAcRaw = -((float)irRaw - irDc_);
    float redAcRaw = -((float)redRaw - redDc_);
    
    irAc_ = irAc_ * 0.85f + irAcRaw * 0.15f;
    redAc_ = redAc_ * 0.85f + redAcRaw * 0.15f;
    
    if (irAc_ > displayAmpMax_) displayAmpMax_ = irAc_;
    if (irAc_ < displayAmpMin_) displayAmpMin_ = irAc_;
    // 0.995の減衰により約1.4秒かけて半減する。これにより画面上のAmp値のブレを完全に防ぐ。
    displayAmpMax_ *= 0.995f;
    displayAmpMin_ *= 0.995f;
    displayAmplitude_ = displayAmpMax_ - displayAmpMin_;
    
    bool beatDetected = false;
    
    static float avgAmp = 0.0f;
    if (sampleCount_ < 10) avgAmp = 0.0f; // リセット時
    
    // もし1.5秒間波が見つからない場合は、ノイズで閾値が大きすぎてスタックしているとみなし、
    // 閾値を急激に下げる（毎サンプル5%減衰）ことで、0.5秒以内に確実に波を再キャッチする。
    if (lastPeakTimeMs_ != 0 && timestampMs - lastPeakTimeMs_ > 1500) {
        avgAmp *= 0.95f; 
    }
    
    // ヒステリシス閾値: 直近の平均振幅の30%、ただし最低5.0
    float threshold = std::max(5.0f, avgAmp * 0.3f);
    
    if (waitingForPeak_) {
        if (irAc_ > irMax_) {
            irMax_ = irAc_;
            redMax_ = redAc_; // 頂点を更新
        } else if (irAc_ < irMax_ - threshold) {
            // ピーク（頂上から閾値分下がった）確定
            if (timestampMs - lastPeakTimeMs_ > 300) { // 不応期300ms(最大200BPM)
                beatDetected = true;
                
                if (lastBeatTimeMs_ != 0) {
                    uint32_t interval = timestampMs - lastBeatTimeMs_;
                    if (interval >= 300 && interval <= 2000) {
                        lastInterval_ = interval;
                        float instantBpm = 60000.0f / (float)interval;
                        
                        if (heartRate_ == 0.0f) {
                            heartRate_ = instantBpm;
                        } else {
                            heartRate_ = heartRate_ * 0.8f + instantBpm * 0.2f;
                        }
                        
                        float irAmplitude = irMax_ - irMin_;
                        float redAmplitude = redMax_ - redMin_;
                        
                        if (avgAmp == 0.0f) avgAmp = irAmplitude;
                        else avgAmp = avgAmp * 0.8f + irAmplitude * 0.2f;
                        
                        perfusionIndex_ = (irDc_ > 0) ? (irAmplitude / irDc_) * 100.0f : 0.0f;
                        
                        if (sampleCount_ > 200 &&
                            irAmplitude > 20.0f && redAmplitude > 20.0f &&
                            irDc_ > 0 && redDc_ > 0 && 
                            perfusionIndex_ >= 0.2f && perfusionIndex_ <= 20.0f) {
                            
                            float r = (redAmplitude / redDc_) / (irAmplitude / irDc_);
                            if (r >= 0.02f && r <= 1.84f) {
                                float instantSpo2;
                                if (r < 0.4f) {
                                    instantSpo2 = 100.0f;
                                } else {
                                    instantSpo2 = -45.060f * r * r + 30.354f * r + 94.845f;
                                    instantSpo2 = std::max(0.0f, std::min(100.0f, instantSpo2));
                                }
                                
                                if (spo2_ == 0.0f) {
                                    spo2_ = instantSpo2;
                                } else {
                                    spo2_ = spo2_ * 0.95f + instantSpo2 * 0.05f;
                                }
                            }
                        }
                    }
                }
                lastBeatTimeMs_ = timestampMs;
                lastPeakTimeMs_ = timestampMs;
            }
            
            // ピーク確定後は「谷探し」状態へ移行
            waitingForPeak_ = false;
            irMin_ = irAc_; // 谷を探す起点として現在値で初期化
            redMin_ = redAc_;
        }
    } else {
        // 谷を探している状態
        if (irAc_ < irMin_) {
            irMin_ = irAc_;
            redMin_ = redAc_; // 谷底を更新
        } else if (irAc_ > irMin_ + threshold) {
            // 谷底から閾値分上がったため「谷」確定。次のピーク探しへ移行。
            waitingForPeak_ = true;
            irMax_ = irAc_; // 次のピークを探す起点として現在値で初期化
            redMax_ = redAc_;
        }
    }
    
    
    
    irAcPrev_ = irAc_;
    
    // --- タイムアウト処理（フリーズ防止） ---
    if (lastBeatTimeMs_ != 0 && timestampMs - lastBeatTimeMs_ > 3000) {
        heartRate_ = 0.0f;
        spo2_ = 0.0f;
        lastBeatTimeMs_ = 0;
        waitingForPeak_ = true; // ピーク検出も初期状態にリセット
        irMax_ = irAc_;
        irMin_ = irAc_;
        redMax_ = redAc_;
        redMin_ = redAc_;
    }
    
    // --- DPTの更新 ---
    updateDpt(irAc_, redAc_);
    
    if (bufferFilled_ >= DPT_PERIOD_MAX * 2 &&
        (timestampMs - lastDptTimeMs_ >= 500 || lastDptTimeMs_ == 0)) {
        analyzeDptSpectrum();
        lastDptTimeMs_ = timestampMs;
    }
    
    return beatDetected;
}

void PulseAnalyzer::updateDpt(float irAc, float redAc) {
    irAcBuffer_[bufferIdx_] = irAc;
    redAcBuffer_[bufferIdx_] = redAc;
    
    bufferIdx_ = (bufferIdx_ + 1) % DPT_BUFFER_SIZE;
    if (bufferFilled_ < DPT_BUFFER_SIZE) {
        bufferFilled_++;
    }
}

void PulseAnalyzer::analyzeDptSpectrum() {
    int N = std::min(bufferFilled_, DPT_BUFFER_SIZE);
    
    float bestPower = 0.0f;
    float bestIrPower = 0.0f;
    float bestRedPower = 0.0f;
    int bestPeriod = 0;
    
    for (int pi = 0; pi < DPT_NUM_PERIODS; pi++) {
        int period = DPT_PERIOD_MIN + pi;
        float omega = 2.0f * (float)M_PI / (float)period;
        float coeff = 2.0f * std::cos(omega);
        
        float s0_ir = 0.0f, s1_ir = 0.0f, s2_ir = 0.0f;
        float s0_red = 0.0f, s1_red = 0.0f, s2_red = 0.0f;
        
        int analyzeLen = std::min(N, period * 3);
        
        float sumIr = 0.0f, sumRed = 0.0f;
        for (int i = 0; i < analyzeLen; i++) {
            int idx = (bufferIdx_ - analyzeLen + i + DPT_BUFFER_SIZE) % DPT_BUFFER_SIZE;
            sumIr += irAcBuffer_[idx];
            sumRed += redAcBuffer_[idx];
        }
        float meanIr = sumIr / analyzeLen;
        float meanRed = sumRed / analyzeLen;
        
        for (int i = 0; i < analyzeLen; i++) {
            int idx = (bufferIdx_ - analyzeLen + i + DPT_BUFFER_SIZE) % DPT_BUFFER_SIZE;
            
            s0_ir = (irAcBuffer_[idx] - meanIr) + coeff * s1_ir - s2_ir;
            s2_ir = s1_ir;
            s1_ir = s0_ir;
            
            s0_red = (redAcBuffer_[idx] - meanRed) + coeff * s1_red - s2_red;
            s2_red = s1_red;
            s1_red = s0_red;
        }
        
        float irPower = s1_ir * s1_ir + s2_ir * s2_ir - coeff * s1_ir * s2_ir;
        float redPower = s1_red * s1_red + s2_red * s2_red - coeff * s1_red * s2_red;
        
        float normFactor = 1.0f / ((float)analyzeLen * (float)analyzeLen);
        irPower *= normFactor;
        redPower *= normFactor;
        
        float totalPower = irPower + redPower;
        
        if (totalPower > bestPower) {
            bestPower = totalPower;
            bestPeriod = period;
            bestIrPower = irPower;
            bestRedPower = redPower;
        }
    }
    
    if (bestPeriod > 0 && bestPower > 0.0f) {
        float dptBpm = 6000.0f / (float)bestPeriod;
        
        if (dptBpm >= 30.0f && dptBpm <= 240.0f) {
            if (dptHeartRate_ == 0.0f) {
                dptHeartRate_ = dptBpm;
            } else {
                dptHeartRate_ = dptHeartRate_ * 0.7f + dptBpm * 0.3f;
            }
        }
        
        if (irDc_ > 0 && redDc_ > 0 && bestIrPower > 0) {
            float irAmpDpt = std::sqrt(bestIrPower);
            float redAmpDpt = std::sqrt(bestRedPower);
            float rDpt = (redAmpDpt / redDc_) / (irAmpDpt / irDc_);
            
            if (rDpt >= 0.02f && rDpt <= 1.84f) {
                float instantSpo2;
                if (rDpt < 0.4f) {
                    instantSpo2 = 100.0f;
                } else {
                    instantSpo2 = -45.060f * rDpt * rDpt + 30.354f * rDpt + 94.845f;
                    instantSpo2 = std::max(0.0f, std::min(100.0f, instantSpo2));
                }
                
                if (dptSpo2_ == 0.0f) {
                    dptSpo2_ = instantSpo2;
                } else {
                    dptSpo2_ = dptSpo2_ * 0.95f + instantSpo2 * 0.05f;
                }
            }
        }
    }
}

} // namespace utils
