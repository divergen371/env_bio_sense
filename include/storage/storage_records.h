#pragma once

#include <cstdint>

namespace storage {

constexpr uint32_t FRAM_MAGIC = 0x4652414D; // "FRAM"
constexpr uint16_t FRAM_FORMAT_VERSION = 1;

enum class EventCode : uint16_t {
    Boot = 0x01,
    SdMountFailed = 0x02,
    SdWriteFailed = 0x03,
    SdRecovered = 0x04,
    RingBufferFull = 0x05,
    SensorError = 0x06
};

// バイトアライメントをパックして無駄な隙間をなくす
#pragma pack(push, 1)

struct FramSuperblock {
    uint32_t magic;
    uint16_t formatVersion;

    uint16_t writeIndex;
    uint16_t readIndex;

    uint32_t nextSequence;
    uint32_t bootCount;

    uint32_t lastSdFlushSequence;

    uint16_t crc16;
};

enum SensorValidFlags : uint32_t {
    VALID_TEMP     = 1u << 0,
    VALID_HUMIDITY = 1u << 1,
    VALID_PRESSURE = 1u << 2,
    VALID_CO2      = 1u << 3,
    VALID_VOC      = 1u << 4,
    VALID_NOX      = 1u << 5,
    VALID_HR       = 1u << 6,
    VALID_SPO2     = 1u << 7,
};

struct EnvironmentalRecord {
    uint32_t sequence;
    uint32_t uptimeMs;

    float temperatureC;
    float humidityRh;
    float pressureHpa;

    uint16_t co2Ppm;

    float vocIndex;
    float noxIndex;

    float heartRateBpm;
    float spo2Percent;

    uint32_t validFlags;
}; // 42 bytes

struct FramRecordHeader {
    uint32_t sequence;
    uint16_t length;
    uint16_t crc;
    uint8_t committed;
}; // 9 bytes

// 結合して FRAM に書き込む完全なレコード
struct PersistentRecord {
    FramRecordHeader header;
    EnvironmentalRecord data;
}; // 51 bytes

struct EventRecord {
    FramRecordHeader header;
    uint32_t uptimeMs;
    uint16_t eventCode;
    int32_t detail;
}; // 9 + 10 = 19 bytes

#pragma pack(pop)

// 定数定義 (アドレスマップ)
constexpr uint16_t ADDR_SUPERBLOCK    = 0x0000;
constexpr uint16_t ADDR_STATE         = 0x0100;
constexpr uint16_t ADDR_EVENT_JOURNAL = 0x0200;
constexpr uint16_t ADDR_RING_BUFFER   = 0x1000;

// 容量とレコードサイズの定義
constexpr size_t FRAM_CAPACITY        = 32768; // 32KB
constexpr size_t RECORD_SLOT_SIZE     = 64;    // 余裕を持たせた64バイト固定枠
constexpr size_t RING_BUFFER_SIZE     = FRAM_CAPACITY - ADDR_RING_BUFFER; // 28672 bytes
constexpr size_t MAX_RECORDS          = RING_BUFFER_SIZE / RECORD_SLOT_SIZE; // 448 records

} // namespace storage
