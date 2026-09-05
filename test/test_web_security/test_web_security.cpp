#include <unity.h>
#include "services/web_security.h"

using namespace services::web_security;

void test_normalizes_root_name() {
    char output[128] {};
    TEST_ASSERT_TRUE(normalizeVisibleDataPath(
        "log_20260906_v7.csv", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("/log_20260906_v7.csv", output);
}

void test_accepts_ppg_nested_files() {
    char output[128] {};
    TEST_ASSERT_TRUE(normalizeVisibleDataPath(
        "/data/ppg/2026-09-06/010203/raw.ppg", output,
        sizeof(output)));
}

void test_accepts_recovery_evidence() {
    char output[128] {};
    TEST_ASSERT_TRUE(normalizeVisibleDataPath(
        "/data/ppg/2026-09-06/010203/raw.ppg.incomplete",
        output, sizeof(output)));
}

void test_rejects_traversal_and_encoded_separator() {
    char output[128] {};
    TEST_ASSERT_FALSE(normalizeVisibleDataPath(
        "/data/ppg/../secrets.h", output, sizeof(output)));
    TEST_ASSERT_FALSE(normalizeVisibleDataPath(
        "/data%2Fppg/raw.ppg", output, sizeof(output)));
    TEST_ASSERT_FALSE(normalizeVisibleDataPath(
        "/data\\ppg\\raw.ppg", output, sizeof(output)));
}

void test_rejects_unapproved_nested_roots() {
    char output[128] {};
    TEST_ASSERT_FALSE(normalizeVisibleDataPath(
        "/private/config.json", output, sizeof(output)));
}

void test_rejects_temporary_and_unknown_files() {
    char output[128] {};
    TEST_ASSERT_FALSE(normalizeVisibleDataPath(
        "/data/ppg/2026-09-06/010203/raw.ppg.tmp",
        output, sizeof(output)));
    TEST_ASSERT_FALSE(normalizeVisibleDataPath(
        "/firmware.bin", output, sizeof(output)));
}

void test_path_inside_requires_segment_boundary() {
    TEST_ASSERT_TRUE(isPathInside(
        "/data/ppg/2026-09-06/010203/raw.ppg",
        "/data/ppg/2026-09-06/010203"));
    TEST_ASSERT_FALSE(isPathInside(
        "/data/ppg/2026-09-06/0102034/raw.ppg",
        "/data/ppg/2026-09-06/010203"));
}

void test_constant_time_compare_contract() {
    TEST_ASSERT_TRUE(constantTimeEquals("token-123", "token-123"));
    TEST_ASSERT_FALSE(constantTimeEquals("token-12", "token-123"));
    TEST_ASSERT_FALSE(constantTimeEquals("token-124", "token-123"));
    TEST_ASSERT_FALSE(constantTimeEquals(nullptr, "token-123"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_normalizes_root_name);
    RUN_TEST(test_accepts_ppg_nested_files);
    RUN_TEST(test_accepts_recovery_evidence);
    RUN_TEST(test_rejects_traversal_and_encoded_separator);
    RUN_TEST(test_rejects_unapproved_nested_roots);
    RUN_TEST(test_rejects_temporary_and_unknown_files);
    RUN_TEST(test_path_inside_requires_segment_boundary);
    RUN_TEST(test_constant_time_compare_contract);
    return UNITY_END();
}
