#include <unity.h>
#include "storage/ppg_binary_v1.h"
#include <cstring>

void setUp(void) {}
void tearDown(void) {}

namespace {

size_t buildGolden(uint8_t* output, size_t capacity) {
    using namespace storage::ppg1;
    const size_t required = FILE_HEADER_SIZE + BLOCK_HEADER_SIZE +
        2 * SAMPLE_SIZE + FOOTER_SIZE;
    if (capacity < required) return 0;

    FileHeader file;
    file.startUnixUs = UINT64_C(1788652800123456);
    file.sampleRateHz = 100;
    file.sampleAverage = 4;
    file.pulseWidthUs = 411;
    file.adcRangeNa = 4096;
    file.redLedCurrentX10Ma = 64;
    file.irLedCurrentX10Ma = 64;
    file.blockPayloadMax = 4096;
    file.sessionIdHash = fnv1a64("20260906T000000Z");
    encodeFileHeader(file, output);

    uint8_t* payload = output + FILE_HEADER_SIZE + BLOCK_HEADER_SIZE;
    encodeSample(0x12345, 0x23456, payload);
    encodeSample(0x3FFFF, 0x45678, payload + SAMPLE_SIZE);
    BlockHeader block;
    block.blockIndex = 0;
    block.firstSampleIndex = 0;
    block.sampleCount = 2;
    block.payloadSize = 2 * SAMPLE_SIZE;
    block.payloadCrc32 = crc32(payload, block.payloadSize);
    encodeBlockHeader(block, output + FILE_HEADER_SIZE);

    Crc32 stream;
    stream.update(payload, block.payloadSize);
    Footer footer;
    footer.endUnixUs = file.startUnixUs + 20000;
    footer.blockCount = 1;
    footer.sampleCount = 2;
    footer.droppedSamples = 0;
    footer.fifoOverflows = 0;
    footer.streamCrc32 = stream.value();
    encodeFooter(footer, payload + block.payloadSize);
    return required;
}

} // namespace

void test_crc32_standard_vector(void) {
    const uint8_t input[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926,
        storage::ppg1::crc32(input, sizeof(input) - 1));
}

void test_golden_file_roundtrip(void) {
    uint8_t bytes[256] {};
    const size_t size = buildGolden(bytes, sizeof(bytes));
    TEST_ASSERT_EQUAL_UINT32(148, size);
    TEST_ASSERT_EQUAL_MEMORY("PPG1", bytes, 4);
    TEST_ASSERT_EQUAL_MEMORY("BLK1", bytes + 64, 4);
    TEST_ASSERT_EQUAL_MEMORY("END1", bytes + 108, 4);
    const storage::ppg1::ScanResult result =
        storage::ppg1::scan(bytes, size);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::ppg1::Error::None),
        static_cast<uint8_t>(result.error));
    TEST_ASSERT_TRUE(result.complete);
    TEST_ASSERT_EQUAL_UINT32(1, result.blockCount);
    TEST_ASSERT_EQUAL_UINT32(2, result.sampleCount);
}

void test_masks_unused_sample_bits(void) {
    uint8_t sample[storage::ppg1::SAMPLE_SIZE] {};
    storage::ppg1::encodeSample(0xFFFFFFFFu, 0xABC45678u, sample);
    TEST_ASSERT_EQUAL_HEX32(0x0003FFFFu,
        storage::ppg1::getU32(sample));
    TEST_ASSERT_EQUAL_HEX32(0x00005678u,
        storage::ppg1::getU32(sample + 4));
}

void test_payload_corruption_stops_at_header(void) {
    uint8_t bytes[256] {};
    const size_t size = buildGolden(bytes, sizeof(bytes));
    bytes[storage::ppg1::FILE_HEADER_SIZE +
          storage::ppg1::BLOCK_HEADER_SIZE + 3] ^= 0x01;
    const storage::ppg1::ScanResult result =
        storage::ppg1::scan(bytes, size);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::ppg1::Error::PayloadCrc),
        static_cast<uint8_t>(result.error));
    TEST_ASSERT_EQUAL_UINT32(storage::ppg1::FILE_HEADER_SIZE,
                             result.lastValidOffset);
}

void test_truncated_payload_is_not_complete(void) {
    uint8_t bytes[256] {};
    const size_t size = buildGolden(bytes, sizeof(bytes));
    const storage::ppg1::ScanResult result =
        storage::ppg1::scan(bytes, size - 45);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::ppg1::Error::TooShort),
        static_cast<uint8_t>(result.error));
    TEST_ASSERT_FALSE(result.complete);
}

void test_footer_mismatch_is_rejected(void) {
    uint8_t bytes[256] {};
    const size_t size = buildGolden(bytes, sizeof(bytes));
    uint8_t* footer = bytes + size - storage::ppg1::FOOTER_SIZE;
    storage::ppg1::putU32(footer + 20, 3);
    storage::ppg1::putU32(footer + 36,
        storage::ppg1::crc32(footer, 36));
    const storage::ppg1::ScanResult result =
        storage::ppg1::scan(bytes, size);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::ppg1::Error::FooterMismatch),
        static_cast<uint8_t>(result.error));
}

void test_unknown_version_is_rejected(void) {
    uint8_t bytes[256] {};
    const size_t size = buildGolden(bytes, sizeof(bytes));
    storage::ppg1::putU16(bytes + 4, 2);
    storage::ppg1::putU32(bytes + 60,
        storage::ppg1::crc32(bytes, 60));
    const storage::ppg1::ScanResult result =
        storage::ppg1::scan(bytes, size);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(storage::ppg1::Error::UnsupportedVersion),
        static_cast<uint8_t>(result.error));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_standard_vector);
    RUN_TEST(test_golden_file_roundtrip);
    RUN_TEST(test_masks_unused_sample_bits);
    RUN_TEST(test_payload_corruption_stops_at_header);
    RUN_TEST(test_truncated_payload_is_not_complete);
    RUN_TEST(test_footer_mismatch_is_rejected);
    RUN_TEST(test_unknown_version_is_rejected);
    return UNITY_END();
}
