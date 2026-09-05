#include <unity.h>
#include "utils/location_policy.h"
#include "utils/location_selector.h"
#include <cmath>

using namespace utils::location_policy;

void test_gnss_quality_contract(void) {
    core::GnssData gnss {};
    gnss.fixValid = true;
    gnss.ageMs = 5000;
    gnss.latitudeDeg = 35.503788;
    gnss.longitudeDeg = 139.650497;
    gnss.satellites = 4;
    gnss.hdopValid = true;
    gnss.hdop = 5.0f;
    TEST_ASSERT_TRUE(gnssFixAcceptable(gnss));

    gnss.ageMs = 5001;
    TEST_ASSERT_FALSE(gnssFixAcceptable(gnss));
    gnss.ageMs = 5000;
    gnss.satellites = 3;
    TEST_ASSERT_FALSE(gnssFixAcceptable(gnss));
    gnss.satellites = 4;
    gnss.hdop = 5.01f;
    TEST_ASSERT_FALSE(gnssFixAcceptable(gnss));
    gnss.hdop = 1.0f;
    gnss.latitudeDeg = NAN;
    TEST_ASSERT_FALSE(gnssFixAcceptable(gnss));
}

void test_coordinate_ranges(void) {
    TEST_ASSERT_TRUE(coordinateValid(-90.0, -180.0));
    TEST_ASSERT_TRUE(coordinateValid(90.0, 180.0));
    TEST_ASSERT_FALSE(coordinateValid(90.0001, 0.0));
    TEST_ASSERT_FALSE(coordinateValid(0.0, 180.0001));
}

void test_distance_and_station_threshold(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f,
        static_cast<float>(distanceMeters(35.0, 139.0, 35.0, 139.0)));
    const double oneKmNorth = 1000.0 / 6371000.0 /
        0.017453292519943295;
    const double distance = distanceMeters(
        35.0, 139.0, 35.0 + oneKmNorth, 139.0);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 1000.0f, static_cast<float>(distance));
    TEST_ASSERT_FALSE(shouldReselectStations(999.999));
    TEST_ASSERT_TRUE(shouldReselectStations(1000.0));
}

core::GnssData goodFix(double latitude, double longitude) {
    core::GnssData gnss {};
    gnss.fixValid = true;
    gnss.ageMs = 100;
    gnss.latitudeDeg = latitude;
    gnss.longitudeDeg = longitude;
    gnss.satellites = 8;
    gnss.hdopValid = true;
    gnss.hdop = 0.9f;
    gnss.sampleMonotonicUs = 900000;
    return gnss;
}

void test_selector_requires_three_consistent_fixes(void) {
    core::DeviceLocation fallback {};
    fallback.latitudeDeg = 35.0;
    fallback.longitudeDeg = 139.0;
    fallback.source = core::LocationSource::ConfiguredFallback;
    fallback.valid = true;
    utils::LocationSelector selector;
    selector.reset(fallback);

    const core::GnssData fix = goodFix(35.5, 139.6);
    selector.update(fix, 1000);
    selector.update(fix, 2000);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::LocationSource::ConfiguredFallback),
        static_cast<uint8_t>(selector.current(2000).source));
    selector.update(fix, 3000);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::LocationSource::GnssLive),
        static_cast<uint8_t>(selector.current(3000).source));
}

void test_selector_live_last_known_fallback_sequence(void) {
    core::DeviceLocation fallback {};
    fallback.latitudeDeg = 35.0;
    fallback.longitudeDeg = 139.0;
    fallback.source = core::LocationSource::ConfiguredFallback;
    fallback.valid = true;
    utils::LocationSelector selector;
    selector.reset(fallback);
    const core::GnssData fix = goodFix(35.5, 139.6);
    selector.update(fix, 1000);
    selector.update(fix, 2000);
    selector.update(fix, 3000);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::LocationSource::GnssLastKnown),
        static_cast<uint8_t>(selector.current(3000 + 5001).source));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::LocationSource::ConfiguredFallback),
        static_cast<uint8_t>(selector.current(
            2900 + LAST_KNOWN_MAX_AGE_MS + 1).source));
}

void test_selector_rejects_inconsistent_third_candidate(void) {
    utils::LocationSelector selector;
    selector.reset({});
    selector.update(goodFix(35.0, 139.0), 1000);
    selector.update(goodFix(35.0, 139.0001), 2000);
    selector.update(goodFix(35.01, 139.01), 3000);
    TEST_ASSERT_FALSE(selector.current(3000).valid);
    selector.update(goodFix(35.01, 139.01), 4000);
    selector.update(goodFix(35.01, 139.01), 5000);
    TEST_ASSERT_TRUE(selector.current(5000).valid);
}

void test_selector_reconfirms_after_fix_loss(void) {
    utils::LocationSelector selector;
    selector.reset({});
    const core::GnssData first = goodFix(35.0, 139.0);
    selector.update(first, 1000);
    selector.update(first, 2000);
    selector.update(first, 3000);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::LocationSource::GnssLive),
        static_cast<uint8_t>(selector.current(3000).source));

    core::GnssData lost = first;
    lost.fixValid = false;
    selector.update(lost, 4000);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::LocationSource::GnssLastKnown),
        static_cast<uint8_t>(selector.current(4000).source));

    const core::GnssData recovered = goodFix(35.001, 139.001);
    selector.update(recovered, 5000);
    selector.update(recovered, 6000);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::LocationSource::GnssLastKnown),
        static_cast<uint8_t>(selector.current(6000).source));
    selector.update(recovered, 7000);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(core::LocationSource::GnssLive),
        static_cast<uint8_t>(selector.current(7000).source));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_gnss_quality_contract);
    RUN_TEST(test_coordinate_ranges);
    RUN_TEST(test_distance_and_station_threshold);
    RUN_TEST(test_selector_requires_three_consistent_fixes);
    RUN_TEST(test_selector_live_last_known_fallback_sequence);
    RUN_TEST(test_selector_rejects_inconsistent_third_candidate);
    RUN_TEST(test_selector_reconfirms_after_fix_loss);
    return UNITY_END();
}
