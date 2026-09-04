#include <unity.h>

#include "storage/storage_record_codec.h"
#include "storage/storage_records.h"
#include "storage/csv_v7.h"
#include <cstring>

void test_v6_record_fits_existing_slot() {
    TEST_ASSERT_EQUAL_UINT16(119, sizeof(storage::SensorRecordV6));
    TEST_ASSERT_EQUAL_UINT16(128, sizeof(storage::PersistentRecordV6));
}

void test_fixed_point_encoding_rounds_and_saturates() {
    TEST_ASSERT_EQUAL_INT16(136, storage::codec::signedFixed(13.56f, 10.0f));
    TEST_ASSERT_EQUAL_UINT16(10099,
        storage::codec::unsignedFixed(1009.87f, 10.0f));
    TEST_ASSERT_EQUAL_INT16(INT16_MAX,
        storage::codec::signedFixed(99999.0f, 100.0f));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX - 1,
        storage::codec::unsignedFixed(99999.0f, 100.0f));
}

void test_unknown_age_and_large_age_use_distinct_sentinels() {
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,
        storage::codec::ageSeconds(UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX - 1,
        storage::codec::ageSeconds(UINT32_MAX - 1));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,
        storage::codec::milliseconds16(UINT32_MAX));
}

void test_health_pack_round_trip_and_saturates_error_count() {
    const uint8_t packed = storage::codec::packHealth(
        core::DeviceState::RetryWait, core::ErrorCode::BusError, 99);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(core::DeviceState::RetryWait),
        static_cast<uint8_t>(storage::codec::healthState(packed)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(core::ErrorCode::BusError),
        static_cast<uint8_t>(storage::codec::healthError(packed)));
    TEST_ASSERT_EQUAL_UINT8(3,
        storage::codec::healthConsecutiveErrors(packed));
}

void test_source_pack_round_trip() {
    const uint8_t packed = storage::codec::packSources(
        core::TimeSource::Holdover, core::PressureFieldState::LastKnown,
        core::PressureReferenceSource::Amedas);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(core::TimeSource::Holdover),
        static_cast<uint8_t>(storage::codec::timeSource(packed)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(core::PressureFieldState::LastKnown),
        static_cast<uint8_t>(storage::codec::pressureState(packed)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(core::PressureReferenceSource::Amedas),
        static_cast<uint8_t>(storage::codec::pressureSource(packed)));
}

void test_csv_v7_has_fixed_columns_and_blank_missing_values() {
    storage::SensorRecordV6 record {};
    record.sequence = 17;
    record.uptimeMs = 5000;
    record.co2AgeSeconds = UINT16_MAX;
    record.sgp41AgeSeconds = UINT16_MAX;
    record.seaLevelPressureAgeSeconds = UINT16_MAX;
    record.gnssAgeSeconds = UINT16_MAX;
    record.ppsAgeMs = UINT16_MAX;
    record.bme690AgeSeconds = UINT16_MAX;
    record.sht45AgeSeconds = UINT16_MAX;
    record.bmp581AgeSeconds = UINT16_MAX;

    char line[storage::MAX_CSV_LINE_LENGTH + 1] {};
    TEST_ASSERT_TRUE(storage::formatCsvLineV7(line, sizeof(line), record));
    size_t columns = 1;
    for (const char* cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor == ',') ++columns;
    }
    TEST_ASSERT_EQUAL_UINT(storage::CSV_V7_COLUMN_COUNT, columns);
    TEST_ASSERT_NULL(strstr(line, "nan"));
    TEST_ASSERT_NULL(strstr(line, "INVALID"));
}

void test_csv_v7_rejects_truncation() {
    storage::SensorRecordV6 record {};
    char line[32] {};
    TEST_ASSERT_FALSE(storage::formatCsvLineV7(line, sizeof(line), record));
    TEST_ASSERT_EQUAL_CHAR('\0', line[0]);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_v6_record_fits_existing_slot);
    RUN_TEST(test_fixed_point_encoding_rounds_and_saturates);
    RUN_TEST(test_unknown_age_and_large_age_use_distinct_sentinels);
    RUN_TEST(test_health_pack_round_trip_and_saturates_error_count);
    RUN_TEST(test_source_pack_round_trip);
    RUN_TEST(test_csv_v7_has_fixed_columns_and_blank_missing_values);
    RUN_TEST(test_csv_v7_rejects_truncation);
    return UNITY_END();
}
