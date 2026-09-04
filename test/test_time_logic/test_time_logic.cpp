#include <unity.h>
#include "core/sensor_types.h"
#include "utils/utc_time.h"

void test_utc_calendar_conversion_is_timezone_independent() {
    int64_t epochMs = -1;
    TEST_ASSERT_TRUE(utils::utcEpochMsFromCalendar(
        1970, 1, 1, 0, 0, 0, 0, epochMs));
    TEST_ASSERT_EQUAL_INT64(0, epochMs);

    TEST_ASSERT_TRUE(utils::utcEpochMsFromCalendar(
        2026, 9, 5, 0, 0, 0, 0, epochMs));
    TEST_ASSERT_EQUAL_INT64(1788566400000LL, epochMs);

    TEST_ASSERT_TRUE(utils::utcEpochMsFromCalendar(
        2000, 2, 29, 12, 34, 56, 780, epochMs));
    TEST_ASSERT_EQUAL_INT64(951827696780LL, epochMs);
}

void test_utc_calendar_rejects_invalid_fields() {
    int64_t epochMs = 123;
    TEST_ASSERT_FALSE(utils::utcEpochMsFromCalendar(
        2026, 2, 29, 0, 0, 0, 0, epochMs));
    TEST_ASSERT_FALSE(utils::utcEpochMsFromCalendar(
        2026, 13, 1, 0, 0, 0, 0, epochMs));
    TEST_ASSERT_FALSE(utils::utcEpochMsFromCalendar(
        2026, 9, 5, 24, 0, 0, 0, epochMs));
}

void test_ntp_seconds_convert_exactly_once_to_microseconds() {
    int64_t epochUs = 0;
    TEST_ASSERT_TRUE(utils::epochSecondsToMicroseconds(1788566400LL, epochUs));
    TEST_ASSERT_EQUAL_INT64(1788566400000000LL, epochUs);
}

void test_time_source_discipline_contract() {
    TEST_ASSERT_TRUE(core::isDisciplinedTimeSource(core::TimeSource::Gnss));
    TEST_ASSERT_TRUE(core::isDisciplinedTimeSource(core::TimeSource::Ntp));
    TEST_ASSERT_FALSE(core::isDisciplinedTimeSource(core::TimeSource::Holdover));
    TEST_ASSERT_FALSE(core::isDisciplinedTimeSource(core::TimeSource::Manual));
    TEST_ASSERT_FALSE(core::isDisciplinedTimeSource(core::TimeSource::Unset));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(core::TimeSource::Holdover),
        static_cast<uint8_t>(core::timeSourceAfterGnssLoss(
            core::TimeSource::Gnss, true)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(core::TimeSource::Ntp),
        static_cast<uint8_t>(core::timeSourceAfterGnssLoss(
            core::TimeSource::Ntp, true)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(core::TimeSource::Gnss),
        static_cast<uint8_t>(core::timeSourceAfterGnssLoss(
            core::TimeSource::Gnss, false)));
}

void test_pps_age_is_measured_and_safely_saturated() {
    TEST_ASSERT_EQUAL_UINT32(2500, utils::monotonicAgeMs(3500000, 1000000));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, utils::monotonicAgeMs(1000000, 0));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, utils::monotonicAgeMs(1000000, 1000001));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
        utils::monotonicAgeMs(
            static_cast<int64_t>(UINT32_MAX) * 1000LL + 2000LL, 1000LL));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_utc_calendar_conversion_is_timezone_independent);
    RUN_TEST(test_utc_calendar_rejects_invalid_fields);
    RUN_TEST(test_ntp_seconds_convert_exactly_once_to_microseconds);
    RUN_TEST(test_time_source_discipline_contract);
    RUN_TEST(test_pps_age_is_measured_and_safely_saturated);
    return UNITY_END();
}
