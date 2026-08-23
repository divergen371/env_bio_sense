#include <unity.h>
#include <cstdint>

// SCD41 FRC Decode Logic Test
int16_t decodeFrcCorrection(uint16_t rawWord) {
    if (rawWord == 0xFFFF) return 0; // Treat as failure in calling logic
    return static_cast<int16_t>(rawWord) - 0x8000;
}

void test_frc_decode_zero(void) {
    TEST_ASSERT_EQUAL_INT16(0, decodeFrcCorrection(0x8000));
}

void test_frc_decode_negative(void) {
    TEST_ASSERT_EQUAL_INT16(-50, decodeFrcCorrection(0x7FCE));
}

void test_frc_decode_positive(void) {
    TEST_ASSERT_EQUAL_INT16(50, decodeFrcCorrection(0x8032));
}

// FRC Preconditions Logic Test (Mock)
class MockScd41 {
public:
    uint32_t measurementStartMs_ = 0;
    bool hasValidData_ = false;
    bool calibrationInProgress_ = false;
    uint32_t lastAmbientPressureSetMs_ = 0;
    bool hasAmbientPressure_ = false;
    
    bool validatePreconditions(uint32_t nowMs, uint16_t referencePpm) {
        if (calibrationInProgress_) return false;
        
        uint32_t uptimeMs = nowMs - measurementStartMs_;
        if (measurementStartMs_ == 0 || uptimeMs < 180000) return false;
        if (!hasValidData_) return false;
        if (referencePpm < 400 || referencePpm > 5000) return false;
        
        if (hasAmbientPressure_ && (nowMs - lastAmbientPressureSetMs_ > 15000)) return false;
        
        return true;
    }
};

void test_frc_preconditions_success(void) {
    MockScd41 sensor;
    sensor.measurementStartMs_ = 1000;
    sensor.hasValidData_ = true;
    
    // nowMs = 181000, uptime = 180000 (3 mins)
    TEST_ASSERT_TRUE(sensor.validatePreconditions(181000, 450));
}

void test_frc_preconditions_fail_uptime(void) {
    MockScd41 sensor;
    sensor.measurementStartMs_ = 1000;
    sensor.hasValidData_ = true;
    
    // uptime = 179999 (less than 3 mins)
    TEST_ASSERT_FALSE(sensor.validatePreconditions(180999, 450));
}

void test_frc_preconditions_fail_no_valid_data(void) {
    MockScd41 sensor;
    sensor.measurementStartMs_ = 1000;
    sensor.hasValidData_ = false; // No data
    
    TEST_ASSERT_FALSE(sensor.validatePreconditions(181000, 450));
}

void test_frc_preconditions_fail_stale_pressure(void) {
    MockScd41 sensor;
    sensor.measurementStartMs_ = 1000;
    sensor.hasValidData_ = true;
    sensor.hasAmbientPressure_ = true;
    sensor.lastAmbientPressureSetMs_ = 10000;
    
    // current time is 30000, pressure was set at 10000. Age = 20000ms (> 15000ms)
    TEST_ASSERT_FALSE(sensor.validatePreconditions(30000, 450));
}

void test_frc_preconditions_fail_out_of_range(void) {
    MockScd41 sensor;
    sensor.measurementStartMs_ = 1000;
    sensor.hasValidData_ = true;
    
    TEST_ASSERT_FALSE(sensor.validatePreconditions(181000, 399));
    TEST_ASSERT_FALSE(sensor.validatePreconditions(181000, 5001));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_frc_decode_zero);
    RUN_TEST(test_frc_decode_negative);
    RUN_TEST(test_frc_decode_positive);
    
    RUN_TEST(test_frc_preconditions_success);
    RUN_TEST(test_frc_preconditions_fail_uptime);
    RUN_TEST(test_frc_preconditions_fail_no_valid_data);
    RUN_TEST(test_frc_preconditions_fail_stale_pressure);
    RUN_TEST(test_frc_preconditions_fail_out_of_range);
    
    return UNITY_END();
}
