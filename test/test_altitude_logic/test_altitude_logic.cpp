#include <unity.h>
#include "utils/altitude_math.h"
#include "utils/amedas_policy.h"
#include "utils/bmp581_calibration.h"
#include <cmath>

void test_altitude_formula_round_trip(void) {
    float pressure = 0.0f;
    TEST_ASSERT_TRUE(utils::altitude_math::expectedPressureHpa(
        1013.2f, 22.0f, 13.6f, pressure));
    float altitude = 0.0f;
    TEST_ASSERT_TRUE(utils::altitude_math::absoluteAltitudeM(
        pressure, 0.0f, 1013.2f, 22.0f, altitude));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 13.6f, altitude);
}

void test_positive_sensor_offset_is_subtracted(void) {
    float expected = 0.0f;
    TEST_ASSERT_TRUE(utils::altitude_math::expectedPressureHpa(
        1013.2f, 20.0f, 13.6f, expected));
    float altitude = 0.0f;
    TEST_ASSERT_TRUE(utils::altitude_math::absoluteAltitudeM(
        expected + 0.4f, 0.4f, 1013.2f, 20.0f, altitude));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 13.6f, altitude);
}

void test_altitude_rejects_invalid_inputs(void) {
    float altitude = 123.0f;
    TEST_ASSERT_FALSE(utils::altitude_math::absoluteAltitudeM(
        NAN, 0.0f, 1013.2f, 20.0f, altitude));
    TEST_ASSERT_FALSE(utils::altitude_math::absoluteAltitudeM(
        1000.0f, 0.0f, 0.0f, 20.0f, altitude));
}

void test_hysteresis_boundaries(void) {
    using utils::altitude_math::applyDisplayHysteresis;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f,
        applyDisplayHysteresis(12.0f, NAN, false));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f,
        applyDisplayHysteresis(10.5f, 10.0f, true));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.1f,
        applyDisplayHysteresis(10.6f, 10.0f, true));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.9f,
        applyDisplayHysteresis(9.4f, 10.0f, true));
}

void test_amedas_timestamp_and_age(void) {
    int64_t observationMs = 0;
    TEST_ASSERT_TRUE(utils::amedas_policy::parseJstIso8601(
        "2026-09-05T12:30:00+09:00", observationMs));
    int64_t expectedMs = 0;
    TEST_ASSERT_TRUE(utils::utcEpochMsFromCalendar(
        2026, 9, 5, 3, 30, 0, 0, expectedMs));
    TEST_ASSERT_EQUAL_INT64(expectedMs, observationMs);
    uint32_t ageMs = 0;
    TEST_ASSERT_TRUE(utils::amedas_policy::observationAgeMs(
        observationMs + 900000, observationMs, ageMs));
    TEST_ASSERT_EQUAL_UINT32(900000, ageMs);
    TEST_ASSERT_FALSE(utils::amedas_policy::observationAgeMs(
        observationMs + 900001, observationMs, ageMs));
}

void test_idw_rejects_bad_quality_and_needs_three(void) {
    utils::amedas_policy::StationSample samples[4] = {
        {1010.0f, 10.0f, 0, true},
        {1012.0f, 20.0f, 0, true},
        {1000.0f, 5.0f, 1, true},
        {NAN, 3.0f, 0, true}
    };
    utils::amedas_policy::IdwResult result;
    TEST_ASSERT_FALSE(utils::amedas_policy::interpolateIdw(samples, 4, result));
    samples[2].qualityCode = 0;
    TEST_ASSERT_TRUE(utils::amedas_policy::interpolateIdw(samples, 4, result));
    TEST_ASSERT_EQUAL_UINT8(3, result.usedStations);
    TEST_ASSERT_TRUE(result.seaLevelPressureHpa >= 1000.0f);
    TEST_ASSERT_TRUE(result.seaLevelPressureHpa <= 1012.0f);
}

void test_idw_exact_station_stays_in_range(void) {
    utils::amedas_policy::StationSample samples[3] = {
        {1008.0f, 0.0f, 0, true},
        {1010.0f, 10.0f, 0, true},
        {1012.0f, 20.0f, 0, true}
    };
    utils::amedas_policy::IdwResult result;
    TEST_ASSERT_TRUE(utils::amedas_policy::interpolateIdw(samples, 3, result));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1008.0f, result.seaLevelPressureHpa);
}

void test_calibration_accepts_known_positive_offset(void) {
    using namespace utils::bmp581_calibration;
    static Sample samples[EXPECTED_SAMPLES];
    for (uint32_t i = 0; i < EXPECTED_SAMPLES; ++i) {
        const float temperatureC = 18.0f + (i % 21u) * 0.01f;
        const float seaLevelHpa = 1011.5f + (i % 11u) * 0.001f;
        float expectedHpa = NAN;
        TEST_ASSERT_TRUE(utils::altitude_math::expectedPressureHpa(
            seaLevelHpa, temperatureC, REFERENCE_ALTITUDE_M, expectedHpa));
        const float noiseHpa =
            (static_cast<int>(i % 9u) - 4) * 0.001f;
        TEST_ASSERT_TRUE(makeSample(expectedHpa + 0.4f + noiseHpa,
            temperatureC, seaLevelHpa, samples[i]));
    }
    Result result;
    TEST_ASSERT_TRUE(evaluate(samples, EXPECTED_SAMPLES,
                              EXPECTED_SAMPLES, result));
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.4f, result.pressureOffsetHpa);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, REFERENCE_ALTITUDE_M,
                             result.postCalibrationMeanAltitudeM);
    TEST_ASSERT_TRUE(result.postCalibrationStdDevM < 0.5f);
}

void test_calibration_rejects_less_than_eighty_percent(void) {
    using namespace utils::bmp581_calibration;
    static Sample samples[EXPECTED_SAMPLES];
    const uint32_t validCount = 2399u;
    for (uint32_t i = 0; i < validCount; ++i) {
        samples[i] = {0.4f, 20.0f, 1013.0f};
    }
    Result result;
    TEST_ASSERT_FALSE(evaluate(samples, validCount,
                               EXPECTED_SAMPLES, result));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(Failure::InsufficientSamples),
        static_cast<uint8_t>(result.failure));
}

void test_calibration_rejects_excessive_noise(void) {
    using namespace utils::bmp581_calibration;
    static Sample samples[EXPECTED_SAMPLES];
    for (uint32_t i = 0; i < EXPECTED_SAMPLES; ++i) {
        samples[i] = {
            (i & 1u) ? 0.12f : -0.12f,
            20.0f,
            1013.0f
        };
    }
    Result result;
    TEST_ASSERT_FALSE(evaluate(samples, EXPECTED_SAMPLES,
                               EXPECTED_SAMPLES, result));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(Failure::AltitudeNoiseTooHigh),
        static_cast<uint8_t>(result.failure));
}

void test_calibration_reference_is_fixed_at_13_6m(void) {
    using namespace utils::bmp581_calibration;
    TEST_ASSERT_TRUE(validReferenceAltitude(13.6f));
    TEST_ASSERT_FALSE(validReferenceAltitude(12.0f));
    TEST_ASSERT_FALSE(validReferenceAltitude(NAN));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_altitude_formula_round_trip);
    RUN_TEST(test_positive_sensor_offset_is_subtracted);
    RUN_TEST(test_altitude_rejects_invalid_inputs);
    RUN_TEST(test_hysteresis_boundaries);
    RUN_TEST(test_amedas_timestamp_and_age);
    RUN_TEST(test_idw_rejects_bad_quality_and_needs_three);
    RUN_TEST(test_idw_exact_station_stays_in_range);
    RUN_TEST(test_calibration_accepts_known_positive_offset);
    RUN_TEST(test_calibration_rejects_less_than_eighty_percent);
    RUN_TEST(test_calibration_rejects_excessive_noise);
    RUN_TEST(test_calibration_reference_is_fixed_at_13_6m);
    return UNITY_END();
}
