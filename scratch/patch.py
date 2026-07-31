import re
import sys

def patch():
    with open('src/utils/pulse_analyzer.cpp', 'r') as f:
        content = f.read()

    # 1. Add 3-second timeout
    timeout_code = """    irAcPrev_ = irAc_;
    
    // --- タイムアウト処理（フリーズ防止） ---
    if (lastBeatTimeMs_ != 0 && timestampMs - lastBeatTimeMs_ > 3000) {
        heartRate_ = 0.0f;
        spo2_ = 0.0f;
        lastBeatTimeMs_ = 0;
    }
    
    // 7. スライディングDPTの更新"""
    content = content.replace("    irAcPrev_ = irAc_;\n    \n    // 7. スライディングDPTの更新", timeout_code)

    # 2. Add interval consistency check
    target_beat_block = """                        lastInterval_ = interval;
                        float instantBpm = 60000.0f / (float)interval;
                        
                        if (heartRate_ == 0.0f) {
                            heartRate_ = instantBpm;
                        } else {
                            heartRate_ = heartRate_ * 0.8f + instantBpm * 0.2f;
                        }"""
    replacement_beat_block = """                        bool isValidBeat = true;
                        if (heartRate_ > 0.0f) {
                            float expectedInterval = 60000.0f / heartRate_;
                            float ratio = (float)interval / expectedInterval;
                            if (ratio < 0.7f || ratio > 1.4f) {
                                isValidBeat = false;
                            }
                        }
                        
                        if (isValidBeat) {
                            lastInterval_ = interval;
                            float instantBpm = 60000.0f / (float)interval;
                            
                            if (heartRate_ == 0.0f) {
                                heartRate_ = instantBpm;
                            } else {
                                heartRate_ = heartRate_ * 0.8f + instantBpm * 0.2f;
                            }"""
    content = content.replace(target_beat_block, replacement_beat_block)

    # 3. Close the isValidBeat block BEFORE the Min/Max reset
    target_reset_block = """                        // 次のビートに向けてMin/Maxを現在値でリセット
                        irMin_ = irAc_;
                        irMax_ = irAc_;
                        redMin_ = redAc_;
                        redMax_ = redAc_;
                        
                        lastBeatTimeMs_ = timestampMs;"""
    replacement_reset_block = """                        } // isValidBeat の終了
                        
                        // 次のビートに向けてMin/Maxを現在値でリセット
                        irMin_ = irAc_;
                        irMax_ = irAc_;
                        redMin_ = redAc_;
                        redMax_ = redAc_;
                        
                        // ビート間隔が長すぎた場合でも次回のために打刻はしておく
                        lastBeatTimeMs_ = timestampMs;"""
    content = content.replace(target_reset_block, replacement_reset_block)

    # 4. Modify DPT to use derivative
    target_dpt_mean = """        float sumIr = 0.0f, sumRed = 0.0f;
        for (int i = 0; i < analyzeLen; i++) {
            int idx = (bufferIdx_ - analyzeLen + i + DPT_BUFFER_SIZE) % DPT_BUFFER_SIZE;
            sumIr += irAcBuffer_[idx];
            sumRed += redAcBuffer_[idx];
        }"""
    replacement_dpt_mean = """        float sumIr = 0.0f, sumRed = 0.0f;
        for (int i = 0; i < analyzeLen; i++) {
            int idx = (bufferIdx_ - analyzeLen + i + DPT_BUFFER_SIZE) % DPT_BUFFER_SIZE;
            int prev_idx = (idx - 1 + DPT_BUFFER_SIZE) % DPT_BUFFER_SIZE;
            sumIr += (irAcBuffer_[idx] - irAcBuffer_[prev_idx]);
            sumRed += (redAcBuffer_[idx] - redAcBuffer_[prev_idx]);
        }"""
    content = content.replace(target_dpt_mean, replacement_dpt_mean)

    target_dpt_calc = """        for (int i = 0; i < analyzeLen; i++) {
            int idx = (bufferIdx_ - analyzeLen + i + DPT_BUFFER_SIZE) % DPT_BUFFER_SIZE;
            
            s0_ir = (irAcBuffer_[idx] - meanIr) + coeff * s1_ir - s2_ir;
            s2_ir = s1_ir;
            s1_ir = s0_ir;
            
            s0_red = (redAcBuffer_[idx] - meanRed) + coeff * s1_red - s2_red;
            s2_red = s1_red;
            s1_red = s0_red;
        }"""
    replacement_dpt_calc = """        for (int i = 0; i < analyzeLen; i++) {
            int idx = (bufferIdx_ - analyzeLen + i + DPT_BUFFER_SIZE) % DPT_BUFFER_SIZE;
            int prev_idx = (idx - 1 + DPT_BUFFER_SIZE) % DPT_BUFFER_SIZE;
            float diff_ir = irAcBuffer_[idx] - irAcBuffer_[prev_idx];
            float diff_red = redAcBuffer_[idx] - redAcBuffer_[prev_idx];
            
            s0_ir = (diff_ir - meanIr) + coeff * s1_ir - s2_ir;
            s2_ir = s1_ir;
            s1_ir = s0_ir;
            
            s0_red = (diff_red - meanRed) + coeff * s1_red - s2_red;
            s2_red = s1_red;
            s1_red = s0_red;
        }"""
    content = content.replace(target_dpt_calc, replacement_dpt_calc)

    with open('src/utils/pulse_analyzer.cpp', 'w') as f:
        f.write(content)

    print("Patched successfully")

if __name__ == "__main__":
    patch()
