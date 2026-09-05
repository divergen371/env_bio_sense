#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace storage {
namespace ppg1 {

constexpr uint16_t VERSION = 1;
constexpr size_t FILE_HEADER_SIZE = 64;
constexpr size_t BLOCK_HEADER_SIZE = 28;
constexpr size_t SAMPLE_SIZE = 8;
constexpr size_t FOOTER_SIZE = 40;
constexpr uint32_t FILE_FLAGS = 0x00000007u;
constexpr uint16_t CHANNEL_MASK_RED_IR = 0x0003u;
constexpr uint32_t MAX30102_VALUE_MASK = 0x0003FFFFu;

enum class Error : uint8_t {
    None = 0,
    TooShort,
    InvalidMagic,
    UnsupportedVersion,
    InvalidSize,
    ReservedField,
    InvalidFlags,
    HeaderCrc,
    BlockSequence,
    SampleSequence,
    PayloadCrc,
    FooterCrc,
    FooterMismatch,
    MissingFooter,
    TrailingData
};

inline const char* errorName(Error error) {
    switch (error) {
        case Error::None: return "NONE";
        case Error::TooShort: return "TOO_SHORT";
        case Error::InvalidMagic: return "INVALID_MAGIC";
        case Error::UnsupportedVersion: return "UNSUPPORTED_VERSION";
        case Error::InvalidSize: return "INVALID_SIZE";
        case Error::ReservedField: return "RESERVED_FIELD";
        case Error::InvalidFlags: return "INVALID_FLAGS";
        case Error::HeaderCrc: return "HEADER_CRC";
        case Error::BlockSequence: return "BLOCK_SEQUENCE";
        case Error::SampleSequence: return "SAMPLE_SEQUENCE";
        case Error::PayloadCrc: return "PAYLOAD_CRC";
        case Error::FooterCrc: return "FOOTER_CRC";
        case Error::FooterMismatch: return "FOOTER_MISMATCH";
        case Error::MissingFooter: return "MISSING_FOOTER";
        case Error::TrailingData: return "TRAILING_DATA";
    }
    return "UNKNOWN";
}

struct FileHeader {
    uint64_t startUnixUs {};
    uint16_t sampleRateHz {};
    uint8_t sampleAverage {};
    uint16_t pulseWidthUs {};
    uint16_t adcRangeNa {};
    uint16_t redLedCurrentX10Ma {};
    uint16_t irLedCurrentX10Ma {};
    uint32_t expectedSamples {};
    uint32_t blockPayloadMax {};
    uint64_t sessionIdHash {};
};

struct BlockHeader {
    uint32_t blockIndex {};
    uint32_t firstSampleIndex {};
    uint16_t sampleCount {};
    uint32_t payloadSize {};
    uint32_t payloadCrc32 {};
};

struct Footer {
    uint64_t endUnixUs {};
    uint32_t blockCount {};
    uint32_t sampleCount {};
    uint32_t droppedSamples {};
    uint32_t fifoOverflows {};
    uint32_t streamCrc32 {};
};

struct ScanResult {
    Error error {Error::None};
    bool complete {false};
    size_t lastValidOffset {};
    uint32_t blockCount {};
    uint32_t sampleCount {};
    uint32_t nextLogicalSampleIndex {};
    uint32_t streamCrc32 {};
    Footer footer {};
};

inline void putU16(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8u);
}

inline void putU32(uint8_t* output, uint32_t value) {
    for (uint8_t i = 0; i < 4; ++i) {
        output[i] = static_cast<uint8_t>(value >> (8u * i));
    }
}

inline void putU64(uint8_t* output, uint64_t value) {
    for (uint8_t i = 0; i < 8; ++i) {
        output[i] = static_cast<uint8_t>(value >> (8u * i));
    }
}

inline uint16_t getU16(const uint8_t* input) {
    return static_cast<uint16_t>(input[0]) |
           static_cast<uint16_t>(input[1]) << 8u;
}

inline uint32_t getU32(const uint8_t* input) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(input[i]) << (8u * i);
    }
    return value;
}

inline uint64_t getU64(const uint8_t* input) {
    uint64_t value = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(input[i]) << (8u * i);
    }
    return value;
}

class Crc32 {
public:
    void reset() { state_ = 0xFFFFFFFFu; }

    void update(const uint8_t* data, size_t length) {
        if (data == nullptr) return;
        for (size_t i = 0; i < length; ++i) {
            state_ ^= data[i];
            for (uint8_t bit = 0; bit < 8; ++bit) {
                state_ = (state_ & 1u)
                    ? (state_ >> 1u) ^ 0xEDB88320u
                    : state_ >> 1u;
            }
        }
    }

    uint32_t value() const { return state_ ^ 0xFFFFFFFFu; }

private:
    uint32_t state_ {0xFFFFFFFFu};
};

inline uint32_t crc32(const uint8_t* data, size_t length) {
    Crc32 crc;
    crc.update(data, length);
    return crc.value();
}

inline uint64_t fnv1a64(const char* text) {
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    if (text == nullptr) return hash;
    while (*text != '\0') {
        hash ^= static_cast<uint8_t>(*text++);
        hash *= UINT64_C(0x00000100000001B3);
    }
    return hash;
}

inline bool reservedZero(const uint8_t* bytes, size_t length) {
    if (bytes == nullptr) return false;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

inline void encodeFileHeader(const FileHeader& header,
                             uint8_t output[FILE_HEADER_SIZE]) {
    std::memset(output, 0, FILE_HEADER_SIZE);
    std::memcpy(output, "PPG1", 4);
    putU16(output + 4, VERSION);
    putU16(output + 6, FILE_HEADER_SIZE);
    putU32(output + 8, FILE_FLAGS);
    putU64(output + 12, header.startUnixUs);
    putU16(output + 20, header.sampleRateHz);
    putU16(output + 22, SAMPLE_SIZE);
    putU16(output + 24, CHANNEL_MASK_RED_IR);
    output[26] = header.sampleAverage;
    putU16(output + 28, header.pulseWidthUs);
    putU16(output + 30, header.adcRangeNa);
    putU16(output + 32, header.redLedCurrentX10Ma);
    putU16(output + 34, header.irLedCurrentX10Ma);
    putU32(output + 36, header.expectedSamples);
    putU32(output + 40, header.blockPayloadMax);
    putU64(output + 44, header.sessionIdHash);
    putU32(output + 60, crc32(output, 60));
}

inline Error decodeFileHeader(const uint8_t* input, size_t length,
                              FileHeader& header) {
    if (input == nullptr || length < FILE_HEADER_SIZE) return Error::TooShort;
    if (std::memcmp(input, "PPG1", 4) != 0) return Error::InvalidMagic;
    if (getU16(input + 4) != VERSION) return Error::UnsupportedVersion;
    if (getU16(input + 6) != FILE_HEADER_SIZE ||
        getU16(input + 22) != SAMPLE_SIZE ||
        getU16(input + 24) != CHANNEL_MASK_RED_IR ||
        getU32(input + 40) == 0 ||
        (getU32(input + 40) % SAMPLE_SIZE) != 0) {
        return Error::InvalidSize;
    }
    if (getU32(input + 8) != FILE_FLAGS) return Error::InvalidFlags;
    if (input[27] != 0 || !reservedZero(input + 52, 8)) {
        return Error::ReservedField;
    }
    if (getU32(input + 60) != crc32(input, 60)) return Error::HeaderCrc;
    header.startUnixUs = getU64(input + 12);
    header.sampleRateHz = getU16(input + 20);
    header.sampleAverage = input[26];
    header.pulseWidthUs = getU16(input + 28);
    header.adcRangeNa = getU16(input + 30);
    header.redLedCurrentX10Ma = getU16(input + 32);
    header.irLedCurrentX10Ma = getU16(input + 34);
    header.expectedSamples = getU32(input + 36);
    header.blockPayloadMax = getU32(input + 40);
    header.sessionIdHash = getU64(input + 44);
    if (header.startUnixUs == 0 || header.sampleRateHz == 0 ||
        header.sampleAverage == 0) {
        return Error::InvalidSize;
    }
    return Error::None;
}

inline void encodeSample(uint32_t red, uint32_t ir,
                         uint8_t output[SAMPLE_SIZE]) {
    putU32(output, red & MAX30102_VALUE_MASK);
    putU32(output + 4, ir & MAX30102_VALUE_MASK);
}

inline void encodeBlockHeader(const BlockHeader& block,
                              uint8_t output[BLOCK_HEADER_SIZE]) {
    std::memset(output, 0, BLOCK_HEADER_SIZE);
    std::memcpy(output, "BLK1", 4);
    putU32(output + 4, block.blockIndex);
    putU32(output + 8, block.firstSampleIndex);
    putU16(output + 12, block.sampleCount);
    putU16(output + 14, SAMPLE_SIZE);
    putU32(output + 16, block.payloadSize);
    putU32(output + 20, block.payloadCrc32);
    putU32(output + 24, crc32(output, 24));
}

inline Error decodeBlockHeader(const uint8_t* input, size_t length,
                               uint32_t payloadMax, BlockHeader& block) {
    if (input == nullptr || length < BLOCK_HEADER_SIZE) return Error::TooShort;
    if (std::memcmp(input, "BLK1", 4) != 0) return Error::InvalidMagic;
    block.blockIndex = getU32(input + 4);
    block.firstSampleIndex = getU32(input + 8);
    block.sampleCount = getU16(input + 12);
    block.payloadSize = getU32(input + 16);
    block.payloadCrc32 = getU32(input + 20);
    if (getU16(input + 14) != SAMPLE_SIZE || block.sampleCount == 0 ||
        block.payloadSize !=
            static_cast<uint32_t>(block.sampleCount) * SAMPLE_SIZE ||
        block.payloadSize > payloadMax) {
        return Error::InvalidSize;
    }
    if (getU32(input + 24) != crc32(input, 24)) return Error::HeaderCrc;
    return Error::None;
}

inline void encodeFooter(const Footer& footer,
                         uint8_t output[FOOTER_SIZE]) {
    std::memset(output, 0, FOOTER_SIZE);
    std::memcpy(output, "END1", 4);
    putU16(output + 4, VERSION);
    putU16(output + 6, FOOTER_SIZE);
    putU64(output + 8, footer.endUnixUs);
    putU32(output + 16, footer.blockCount);
    putU32(output + 20, footer.sampleCount);
    putU32(output + 24, footer.droppedSamples);
    putU32(output + 28, footer.fifoOverflows);
    putU32(output + 32, footer.streamCrc32);
    putU32(output + 36, crc32(output, 36));
}

inline Error decodeFooter(const uint8_t* input, size_t length,
                          Footer& footer) {
    if (input == nullptr || length < FOOTER_SIZE) return Error::TooShort;
    if (std::memcmp(input, "END1", 4) != 0) return Error::InvalidMagic;
    if (getU16(input + 4) != VERSION) return Error::UnsupportedVersion;
    if (getU16(input + 6) != FOOTER_SIZE) return Error::InvalidSize;
    if (getU32(input + 36) != crc32(input, 36)) return Error::FooterCrc;
    footer.endUnixUs = getU64(input + 8);
    footer.blockCount = getU32(input + 16);
    footer.sampleCount = getU32(input + 20);
    footer.droppedSamples = getU32(input + 24);
    footer.fifoOverflows = getU32(input + 28);
    footer.streamCrc32 = getU32(input + 32);
    return Error::None;
}

inline ScanResult scan(const uint8_t* data, size_t length) {
    ScanResult result;
    FileHeader fileHeader;
    result.error = decodeFileHeader(data, length, fileHeader);
    if (result.error != Error::None) return result;
    size_t offset = FILE_HEADER_SIZE;
    result.lastValidOffset = offset;
    Crc32 streamCrc;

    while (offset < length) {
        if (length - offset >= 4 &&
            std::memcmp(data + offset, "END1", 4) == 0) {
            result.error = decodeFooter(
                data + offset, length - offset, result.footer);
            if (result.error != Error::None) return result;
            if (length - offset != FOOTER_SIZE) {
                result.error = Error::TrailingData;
                return result;
            }
            if (result.footer.blockCount != result.blockCount ||
                result.footer.sampleCount != result.sampleCount ||
                result.footer.streamCrc32 != streamCrc.value()) {
                result.error = Error::FooterMismatch;
                return result;
            }
            result.complete = true;
            result.streamCrc32 = streamCrc.value();
            result.lastValidOffset = length;
            return result;
        }

        BlockHeader block;
        result.error = decodeBlockHeader(
            data + offset, length - offset,
            fileHeader.blockPayloadMax, block);
        if (result.error != Error::None) return result;
        if (block.blockIndex != result.blockCount) {
            result.error = Error::BlockSequence;
            return result;
        }
        if (result.blockCount != 0 &&
            block.firstSampleIndex < result.nextLogicalSampleIndex) {
            result.error = Error::SampleSequence;
            return result;
        }
        if (length - offset - BLOCK_HEADER_SIZE < block.payloadSize) {
            result.error = Error::TooShort;
            return result;
        }
        const uint8_t* payload = data + offset + BLOCK_HEADER_SIZE;
        if (crc32(payload, block.payloadSize) != block.payloadCrc32) {
            result.error = Error::PayloadCrc;
            return result;
        }
        streamCrc.update(payload, block.payloadSize);
        ++result.blockCount;
        result.sampleCount += block.sampleCount;
        result.nextLogicalSampleIndex =
            block.firstSampleIndex + block.sampleCount;
        offset += BLOCK_HEADER_SIZE + block.payloadSize;
        result.lastValidOffset = offset;
    }
    result.error = Error::MissingFooter;
    result.streamCrc32 = streamCrc.value();
    return result;
}

} // namespace ppg1
} // namespace storage
