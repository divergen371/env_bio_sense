#include <unity.h>

#include "utils/sht4x_protocol.h"

void test_sensirion_crc_known_vector() {
    const uint8_t data[] {0xBE, 0xEF};
    TEST_ASSERT_EQUAL_HEX8(0x92, utils::sht4x::crc8(data, sizeof(data)));
}

void test_sht4x_frame_decodes_valid_temperature_and_humidity() {
    uint8_t frame[] {0x66, 0x66, 0, 0x80, 0x00, 0};
    frame[2] = utils::sht4x::crc8(frame, 2);
    frame[5] = utils::sht4x::crc8(frame + 3, 2);

    float temperatureC = 0.0f;
    float humidityRh = 0.0f;
    TEST_ASSERT_TRUE(utils::sht4x::decodeMeasurement(
        frame, temperatureC, humidityRh));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, temperatureC);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 56.5f, humidityRh);
}

void test_sht4x_frame_rejects_crc_error() {
    uint8_t frame[] {0x66, 0x66, 0, 0x80, 0x00, 0};
    frame[2] = utils::sht4x::crc8(frame, 2);
    frame[5] = utils::sht4x::crc8(frame + 3, 2);
    frame[3] ^= 0x01;

    float temperatureC = 0.0f;
    float humidityRh = 0.0f;
    TEST_ASSERT_FALSE(utils::sht4x::decodeMeasurement(
        frame, temperatureC, humidityRh));
}

void test_sht4x_humidity_is_clamped_to_physical_range() {
    uint8_t frame[] {0x66, 0x66, 0, 0x00, 0x00, 0};
    frame[2] = utils::sht4x::crc8(frame, 2);
    frame[5] = utils::sht4x::crc8(frame + 3, 2);

    float temperatureC = 0.0f;
    float humidityRh = 0.0f;
    TEST_ASSERT_TRUE(utils::sht4x::decodeMeasurement(
        frame, temperatureC, humidityRh));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, humidityRh);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_sensirion_crc_known_vector);
    RUN_TEST(test_sht4x_frame_decodes_valid_temperature_and_humidity);
    RUN_TEST(test_sht4x_frame_rejects_crc_error);
    RUN_TEST(test_sht4x_humidity_is_clamped_to_physical_range);
    return UNITY_END();
}
