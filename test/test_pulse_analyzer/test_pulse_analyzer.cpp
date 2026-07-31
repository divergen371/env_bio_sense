#include <unity.h>
#include "utils/pulse_analyzer.h"
#include <cmath>

void setUp(void) {}
void tearDown(void) {}

// ===================================================================
// サンプリング間隔: 10ms (100Hz)
// MAX30102のレジスタ設定 (sampleRate=400Hz, sampleAverage=4 => 実効100Hz) に一致。
// ===================================================================
static const uint32_t SAMPLE_INTERVAL_MS = 10;

// 1. 純粋な正弦波（理想的な心拍）60BPM
void test_pure_sine_wave(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // 100Hz * 10秒 = 1000サンプル
    for (int i = 0; i < 1000; i++) {
        float signal = 100000.0f - 10000.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 60.0f, hr);
}

// 2. ダイクロティックノッチ（重複波）60BPM
void test_dicrotic_notch(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    for (int i = 0; i < 1000; i++) {
        float primary = std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        float notch = 0.4f * std::sin(2.0f * M_PI * 2.0f * (timeMs / 1000.0f) - 1.0f);
        float signal = 100000.0f - 10000.0f * (primary + notch);
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 60.0f, hr);
}

// 3. ベースライン変動 60BPM
void test_baseline_wander(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // 15秒 = 1500サンプル
    for (int i = 0; i < 1500; i++) {
        float heartbeat = 10000.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        float wander = 50000.0f * std::sin(2.0f * M_PI * 0.1f * (timeMs / 1000.0f));
        float signal = 100000.0f - heartbeat + wander;
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(6.0f, 60.0f, hr);
}

// 4. 高速心拍 150BPM
void test_fast_heart_rate(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    for (int i = 0; i < 1000; i++) {
        float signal = 100000.0f - 10000.0f * std::sin(2.0f * M_PI * 2.5f * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 150.0f, hr);
}

// 5. 小振幅信号（実機レベル）での検出テスト
void test_small_amplitude_signal(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // 振幅200で60BPM
    for (int i = 0; i < 1000; i++) {
        float signal = 100000.0f - 200.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_GREATER_THAN(0.0f, hr);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 60.0f, hr);
}

// 6. 信号消失時に振幅が減衰するテスト
void test_amplitude_decays_without_signal(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // まず信号を入れる (3秒 = 300サンプル)
    for (int i = 0; i < 300; i++) {
        float signal = 100000.0f - 10000.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float ampWithSignal = analyzer.getSignalAmplitude();
    TEST_ASSERT_GREATER_THAN(100.0f, ampWithSignal);
    
    // 信号を平坦にする (5秒 = 500サンプル @100Hz)
    for (int i = 0; i < 500; i++) {
        analyzer.processSample(100000, 100000, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float ampWithoutSignal = analyzer.getSignalAmplitude();
    TEST_ASSERT_LESS_THAN(50.0f, ampWithoutSignal);
}

// 7. DPTによる心拍推定（72BPM）
void test_dpt_heart_rate_72bpm(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // 72 BPM = 1.2Hz
    for (int i = 0; i < 1000; i++) {
        float signal = 100000.0f - 10000.0f * std::sin(2.0f * M_PI * 1.2f * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float dptHr = analyzer.getDptHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 72.0f, dptHr);
}

// 8. DPTがダイクロティックノッチの高調波を無視するテスト
void test_dpt_ignores_harmonic(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    for (int i = 0; i < 1000; i++) {
        float primary = std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        float notch = 0.4f * std::sin(2.0f * M_PI * 2.0f * (timeMs / 1000.0f) - 1.0f);
        float signal = 100000.0f - 10000.0f * (primary + notch);
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float dptHr = analyzer.getDptHeartRateBpm();
    // DPTは基本波60BPMを検出すべき（高調波120BPMではなく）
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 60.0f, dptHr);
}

// 9. 低灌流（PI < 0.2%）でSpO2が更新されないテスト
void test_low_perfusion_blocks_spo2(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // 振幅50 → PI ≈ 0.05% (DC=100000)
    for (int i = 0; i < 1000; i++) {
        float signal = 100000.0f - 50.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    // 低灌流ではSpO2が更新されず0のままであるべき
    TEST_ASSERT_LESS_THAN(1.0f, analyzer.getSpo2Percent());
}

// 10. R < 0.4 のクランプ処理テスト（SpO2 ≈ 100%）
void test_r_ratio_clamp(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // IR振幅=10000, Red振幅=1000 → R ≈ 0.1 → SpO2 should be clamped to 100%
    for (int i = 0; i < 1000; i++) {
        float ir = 100000.0f - 10000.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        float red = 100000.0f - 1000.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)ir, (uint32_t)red, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    TEST_ASSERT_GREATER_OR_EQUAL(99.0f, analyzer.getSpo2Percent());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_pure_sine_wave);
    RUN_TEST(test_dicrotic_notch);
    RUN_TEST(test_baseline_wander);
    RUN_TEST(test_fast_heart_rate);
    RUN_TEST(test_small_amplitude_signal);
    RUN_TEST(test_amplitude_decays_without_signal);
    RUN_TEST(test_dpt_heart_rate_72bpm);
    RUN_TEST(test_dpt_ignores_harmonic);
    RUN_TEST(test_low_perfusion_blocks_spo2);
    RUN_TEST(test_r_ratio_clamp);
    return UNITY_END();
}
