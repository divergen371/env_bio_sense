#include <unity.h>
#include "core/sensor_types.h"
#include "storage/storage_records.h"

using namespace storage;

void test_v5_quality_extension_fits_existing_slot() {
    TEST_ASSERT_EQUAL_UINT16(117, LEGACY_SENSOR_RECORD_V5_SIZE);
    TEST_ASSERT_EQUAL_UINT16(119, sizeof(SensorRecordV5));
    TEST_ASSERT_EQUAL_UINT16(128, sizeof(PersistentRecordV5));
}

void test_scd41_state_uses_only_reserved_flag_bits() {
    uint32_t flags = VALID_TEMP | VALID_CO2;
    uint32_t encoded = flags |
        (static_cast<uint32_t>(core::DeviceState::RetryWait) << SCD41_STATE_SHIFT);

    TEST_ASSERT_EQUAL_UINT32(flags, encoded & ~SCD41_STATE_MASK);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::DeviceState::RetryWait),
        static_cast<uint8_t>((encoded & SCD41_STATE_MASK) >> SCD41_STATE_SHIFT));
}

void test_event_journal_geometry_and_payload() {
    TEST_ASSERT_EQUAL_UINT16(19, sizeof(EventRecord));
    TEST_ASSERT_EQUAL_UINT16(10, EVENT_PAYLOAD_SIZE);
    TEST_ASSERT_EQUAL_UINT16(112, MAX_EVENT_RECORDS);
    TEST_ASSERT_EQUAL_STRING("SCD41_RECOVERED", eventCodeName(EventCode::Scd41Recovered));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_v5_quality_extension_fits_existing_slot);
    RUN_TEST(test_scd41_state_uses_only_reserved_flag_bits);
    RUN_TEST(test_event_journal_geometry_and_payload);
    return UNITY_END();
}
