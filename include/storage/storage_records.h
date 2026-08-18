#pragma once

#include <cstdint>

namespace storage {

constexpr uint32_t FRAM_MAGIC = 0x4652414D; // "FRAM"
constexpr uint16_t FRAM_FORMAT_VERSION = 4;

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
    
    // SGP41 states (for Gas Index Algorithm continuity)
    bool hasValidSgp41State;
    float sgp41VocState0;
    float sgp41VocState1;

    // SCD41 calibration tracking
    uint32_t lastScd41CalibrationEpoch;

    // BMP581 calibration
    bool hasValidBmp581Calibration;
    float bmp581PressureOffsetHpa;
    uint32_t bmp581CalibEpoch;
    float bmp581CalibTempC;
    float bmp581CalibSeaLevelHpa;

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
    VALID_ALTITUDE = 1u << 8,
};

enum GnssValidFlags : uint16_t {
    GNSS_VALID_FIX          = 1u << 0,
    GNSS_VALID_ALTITUDE     = 1u << 1,
    GNSS_VALID_SPEED        = 1u << 2,
    GNSS_VALID_COURSE       = 1u << 3,
    GNSS_VALID_HDOP         = 1u << 4,
    GNSS_VALID_UTC          = 1u << 5,
    GNSS_PPS_RECENT         = 1u << 6,
    GNSS_TIME_DISCIPLINED   = 1u << 7
};

struct SensorRecordV4 {
    uint32_t sequence;
    uint32_t uptimeMs;

    int64_t sampleMonotonicUs;
    int64_t utcEpochMs;

    float temperatureC;
    float humidityRh;
    float pressureHpa;
    uint16_t co2Ppm;

    float vocIndex;
    float noxIndex;

    float heartRateBpm;
    float spo2Percent;
    float altitudeM;

    // GNSS Fields
    int32_t gnssLatitudeE7;
    int32_t gnssLongitudeE7;
    float gnssAltitudeMslM;
    float gnssSpeedMps;
    float gnssCourseDeg;
    float gnssHdop;

    uint32_t gnssAgeMs;
    uint32_t ppsAgeMs;
    uint16_t gnssSatellites;
    uint16_t gnssValidFlags;
    uint8_t timeSource;

    uint32_t validFlags;
}; // Approx 80+ bytes

struct FramRecordHeader {
    uint32_t sequence;
    uint16_t length;
    uint16_t crc;
    uint8_t committed;
}; // 9 bytes

// 結合して FRAM に書き込む完全なレコード
struct PersistentRecordV4 {
    FramRecordHeader header;
    SensorRecordV4 data;
}; // Approx 90+ bytes

struct EventRecord {
    FramRecordHeader header;
    uint32_t uptimeMs;
    uint16_t eventCode;
    int32_t detail;
}; // 19 bytes

#pragma pack(pop)

// 定数定義 (アドレスマップ)
constexpr uint16_t ADDR_SUPERBLOCK    = 0x0000;
constexpr uint16_t ADDR_STATE         = 0x0100;
constexpr uint16_t ADDR_EVENT_JOURNAL = 0x0200;
constexpr uint16_t ADDR_RING_BUFFER   = 0x1000;

// 容量とレコードサイズの定義
constexpr size_t FRAM_CAPACITY        = 32768; // 32KB
constexpr size_t RECORD_SLOT_SIZE     = 128;   // GNSSデータ追加のため128バイトへ拡張
constexpr size_t RING_BUFFER_SIZE     = FRAM_CAPACITY - ADDR_RING_BUFFER; // 28672 bytes
constexpr size_t MAX_RECORDS          = RING_BUFFER_SIZE / RECORD_SLOT_SIZE; // 224 records

static_assert(sizeof(PersistentRecordV4) <= RECORD_SLOT_SIZE, "PersistentRecordV4 exceeds RECORD_SLOT_SIZE");

} // namespace storage
