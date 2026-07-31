#include <unity.h>
#include "utils/pulse_analyzer.h"
#include <cmath>

void setUp(void) {}
void tearDown(void) {}

// ===================================================================
// 重要: サンプリング間隔は40ms (25Hz)
// MAX30102の実効レート(100Hz / sampleAvg=4 = 25Hz)に合わせる。
// テストが100Hzで通っても実機(25Hz)で通らない問題を防ぐため。
// ===================================================================
static const uint32_t SAMPLE_INTERVAL_MS = 40;

// 1. 純粋な正弦波（理想的な心拍）60BPM
void test_pure_sine_wave(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // 25Hz * 10秒 = 250サンプル
    for (int i = 0; i < 250; i++) {
        float signal = 100000.0f - 10000.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 60.0f, hr);
}

// 2. ダイクロティックノッチ（重複波）60BPM
void test_dicrotic_notch(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    for (int i = 0; i < 250; i++) {
        float primary = std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        float notch = 0.4f * std::sin(2.0f * M_PI * 2.0f * (timeMs / 1000.0f) - 1.0f);
        float signal = 100000.0f - 10000.0f * (primary + notch);
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 60.0f, hr);
}

// 3. ベースライン変動 60BPM
void test_baseline_wander(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // 15秒 = 375サンプル
    for (int i = 0; i < 375; i++) {
        float heartbeat = 10000.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        float wander = 50000.0f * std::sin(2.0f * M_PI * 0.1f * (timeMs / 1000.0f));
        float signal = 100000.0f - heartbeat + wander;
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 60.0f, hr);
}

// 4. 高速心拍 150BPM
void test_fast_heart_rate(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    for (int i = 0; i < 250; i++) {
        float signal = 100000.0f - 10000.0f * std::sin(2.0f * M_PI * 2.5f * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 150.0f, hr);
}

// 5. 小振幅信号（実機レベル）での検出テスト
// 実機のAC成分はDCの0.1-0.5%程度 (振幅200-1000)
void test_small_amplitude_signal(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // 振幅200（実機でよくある値）で60BPM
    for (int i = 0; i < 250; i++) {
        float signal = 100000.0f - 200.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float hr = analyzer.getHeartRateBpm();
    // 小振幅でも心拍が検出されること（0でないこと）
    TEST_ASSERT_GREATER_THAN(0.0f, hr);
    TEST_ASSERT_FLOAT_WITHIN(10.0f, 60.0f, hr);
}

// 6. 信号消失時に振幅が減衰するテスト
void test_amplitude_decays_without_signal(void) {
    utils::PulseAnalyzer analyzer;
    uint32_t timeMs = 0;
    // まず信号を入れる (3秒 = 75サンプル)
    for (int i = 0; i < 75; i++) {
        float signal = 100000.0f - 10000.0f * std::sin(2.0f * M_PI * (timeMs / 1000.0f));
        analyzer.processSample((uint32_t)signal, (uint32_t)signal, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float ampWithSignal = analyzer.getSignalAmplitude();
    TEST_ASSERT_GREATER_THAN(100.0f, ampWithSignal);
    
    // 信号を平坦にする (20秒 = 500サンプル @25Hz)
    for (int i = 0; i < 500; i++) {
        analyzer.processSample(100000, 100000, timeMs);
        timeMs += SAMPLE_INTERVAL_MS;
    }
    float ampWithoutSignal = analyzer.getSignalAmplitude();
    TEST_ASSERT_LESS_THAN(50.0f, ampWithoutSignal);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_pure_sine_wave);
    RUN_TEST(test_dicrotic_notch);
    RUN_TEST(test_baseline_wander);
    RUN_TEST(test_fast_heart_rate);
    RUN_TEST(test_small_amplitude_signal);
    RUN_TEST(test_amplitude_decays_without_signal);
    return UNITY_END();
}
