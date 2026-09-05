#include <unity.h>
#include <cstdint>
#include "utils/scd41_maintenance.h"
#include "utils/scd41_policy.h"

using namespace utils::scd41_policy;

void test_frc_decode_zero(void) {
    int16_t correction = 123;
    TEST_ASSERT_TRUE(decodeFrcCorrection(0x8000, correction));
    TEST_ASSERT_EQUAL_INT16(0, correction);
}

void test_frc_decode_negative(void) {
    int16_t correction = 0;
    TEST_ASSERT_TRUE(decodeFrcCorrection(0x7FCE, correction));
    TEST_ASSERT_EQUAL_INT16(-50, correction);
}

void test_frc_decode_positive(void) {
    int16_t correction = 0;
    TEST_ASSERT_TRUE(decodeFrcCorrection(0x8032, correction));
    TEST_ASSERT_EQUAL_INT16(50, correction);
}

void test_frc_decode_rejects_failure_word(void) {
    int16_t correction = 123;
    TEST_ASSERT_FALSE(decodeFrcCorrection(0xFFFF, correction));
    TEST_ASSERT_EQUAL_INT16(123, correction);
}

void test_frc_preconditions_success(void) {
    TEST_ASSERT_TRUE(frcPreconditions(false, true, MIN_FRC_MEASUREMENT_MS,
                                     true, 450, true, 1000));
}

void test_frc_preconditions_fail_uptime(void) {
    TEST_ASSERT_FALSE(frcPreconditions(false, true, MIN_FRC_MEASUREMENT_MS - 1,
                                      true, 450, true, 1000));
}

void test_frc_preconditions_fail_no_valid_data(void) {
    TEST_ASSERT_FALSE(frcPreconditions(false, true, MIN_FRC_MEASUREMENT_MS,
                                      false, 450, true, 1000));
}

void test_frc_preconditions_fail_stale_pressure(void) {
    TEST_ASSERT_FALSE(frcPreconditions(false, true, MIN_FRC_MEASUREMENT_MS,
                                      true, 450, true, MAX_PRESSURE_AGE_MS + 1));
}

void test_frc_preconditions_require_pressure(void) {
    TEST_ASSERT_FALSE(frcPreconditions(false, true, MIN_FRC_MEASUREMENT_MS,
                                      true, 450, false, 0));
}

void test_frc_preconditions_fail_out_of_range(void) {
    TEST_ASSERT_FALSE(frcPreconditions(false, true, MIN_FRC_MEASUREMENT_MS,
                                      true, 399, true, 0));
    TEST_ASSERT_FALSE(frcPreconditions(false, true, MIN_FRC_MEASUREMENT_MS,
                                      true, 5001, true, 0));
}

void test_recovery_policy_escalates_and_caps_backoff(void) {
    TEST_ASSERT_FALSE(recoveryUsesReinit(0));
    TEST_ASSERT_TRUE(recoveryUsesReinit(1));
    TEST_ASSERT_EQUAL_UINT32(30000, recoveryBackoffMs(1));
    TEST_ASSERT_EQUAL_UINT32(120000, recoveryBackoffMs(2));
    TEST_ASSERT_EQUAL_UINT32(300000, recoveryBackoffMs(3));
    TEST_ASSERT_EQUAL_UINT32(300000, recoveryBackoffMs(200));
}

void test_recovery_quarantine_requires_three_good_samples(void) {
    TEST_ASSERT_FALSE(recoveryQuarantineComplete(0));
    TEST_ASSERT_FALSE(recoveryQuarantineComplete(1));
    TEST_ASSERT_FALSE(recoveryQuarantineComplete(2));
    TEST_ASSERT_TRUE(recoveryQuarantineComplete(3));
}

void test_frc_command_order_and_restart_after_frc_failure(void) {
    char order[6] {};
    uint8_t position = 0;
    auto result = utils::scd41_maintenance::runFrcSequence(
        450,
        [&]() { order[position++] = 'P'; return static_cast<uint16_t>(0); },
        [&]() { order[position++] = 'S'; return static_cast<uint16_t>(0); },
        [&]() { order[position++] = 'W'; },
        [&](uint16_t reference, uint16_t& rawWord) {
            order[position++] = 'F';
            TEST_ASSERT_EQUAL_UINT16(450, reference);
            rawWord = 0xFFFF;
            return static_cast<uint16_t>(7);
        },
        [&]() { order[position++] = 'R'; return static_cast<uint16_t>(0); });

    TEST_ASSERT_EQUAL_STRING("PSWFR", order);
    TEST_ASSERT_TRUE(result.frcAttempted);
    TEST_ASSERT_TRUE(result.restartAttempted);
    TEST_ASSERT_EQUAL_UINT16(7, result.frcError);
    TEST_ASSERT_EQUAL_UINT16(0, result.restartError);
}

void test_frc_stop_failure_never_runs_frc_or_restart(void) {
    uint8_t frcCalls = 0;
    uint8_t restartCalls = 0;
    auto result = utils::scd41_maintenance::runFrcSequence(
        450,
        []() { return static_cast<uint16_t>(0); },
        []() { return static_cast<uint16_t>(9); },
        []() {},
        [&](uint16_t, uint16_t&) {
            ++frcCalls;
            return static_cast<uint16_t>(0);
        },
        [&]() {
            ++restartCalls;
            return static_cast<uint16_t>(0);
        });

    TEST_ASSERT_EQUAL_UINT16(9, result.stopError);
    TEST_ASSERT_FALSE(result.frcAttempted);
    TEST_ASSERT_FALSE(result.restartAttempted);
    TEST_ASSERT_EQUAL_UINT8(0, frcCalls);
    TEST_ASSERT_EQUAL_UINT8(0, restartCalls);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_frc_decode_zero);
    RUN_TEST(test_frc_decode_negative);
    RUN_TEST(test_frc_decode_positive);
    RUN_TEST(test_frc_decode_rejects_failure_word);
    
    RUN_TEST(test_frc_preconditions_success);
    RUN_TEST(test_frc_preconditions_fail_uptime);
    RUN_TEST(test_frc_preconditions_fail_no_valid_data);
    RUN_TEST(test_frc_preconditions_fail_stale_pressure);
    RUN_TEST(test_frc_preconditions_require_pressure);
    RUN_TEST(test_frc_preconditions_fail_out_of_range);
    RUN_TEST(test_recovery_policy_escalates_and_caps_backoff);
    RUN_TEST(test_recovery_quarantine_requires_three_good_samples);
    RUN_TEST(test_frc_command_order_and_restart_after_frc_failure);
    RUN_TEST(test_frc_stop_failure_never_runs_frc_or_restart);
    
    return UNITY_END();
}
