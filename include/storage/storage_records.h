#pragma once

#include <cstddef>
#include <cstdint>

namespace storage {

constexpr uint32_t FRAM_MAGIC = 0x4652414D; // "FRAM"
constexpr uint16_t LEGACY_FRAM_FORMAT_VERSION = 5;
constexpr uint16_t FRAM_FORMAT_VERSION = 6;
constexpr uint16_t LEGACY_CSV_SCHEMA_VERSION = 6;
constexpr uint16_t CSV_SCHEMA_VERSION = 7;
constexpr size_t RECORD_SLOT_SIZE = 128;

enum class EventCode : uint16_t {
    Boot = 0x01,
    SdMountFailed = 0x02,
    SdWriteFailed = 0x03,
    SdRecovered = 0x04,
    RingBufferFull = 0x05,
    SensorError = 0x06,
    I2cLockTimeout = 0x10,
    I2cCommunicationError = 0x11,
    Scd41Stale = 0x20,
    Scd41LockTimeout = 0x21,
    Scd41DataReadyError = 0x22,
    Scd41ReadError = 0x23,
    Scd41RecoveryAttempt = 0x24,
    Scd41RecoveryRestarted = 0x25,
    Scd41RecoveryFailed = 0x26,
    Scd41Recovered = 0x27,
    Scd41DriverError = 0x28,
    Scd41Stabilizing = 0x29,
    Scd41FrcSucceeded = 0x2A,
    Scd41FrcFailed = 0x2B,
    Scd41FactoryResetSucceeded = 0x2C,
    Scd41FactoryResetFailed = 0x2D,
    FramRecordCorrupt = 0x30,
    FramCheckpointFailed = 0x31
};

inline const char* eventCodeName(EventCode code) {
    switch (code) {
        case EventCode::Boot: return "BOOT";
        case EventCode::SdMountFailed: return "SD_MOUNT_FAILED";
        case EventCode::SdWriteFailed: return "SD_WRITE_FAILED";
        case EventCode::SdRecovered: return "SD_RECOVERED";
        case EventCode::RingBufferFull: return "RING_BUFFER_FULL";
        case EventCode::SensorError: return "SENSOR_ERROR";
        case EventCode::I2cLockTimeout: return "I2C_LOCK_TIMEOUT";
        case EventCode::I2cCommunicationError: return "I2C_COMMUNICATION_ERROR";
        case EventCode::Scd41Stale: return "SCD41_STALE";
        case EventCode::Scd41LockTimeout: return "SCD41_LOCK_TIMEOUT";
        case EventCode::Scd41DataReadyError: return "SCD41_DATA_READY_ERROR";
        case EventCode::Scd41ReadError: return "SCD41_READ_ERROR";
        case EventCode::Scd41RecoveryAttempt: return "SCD41_RECOVERY_ATTEMPT";
        case EventCode::Scd41RecoveryRestarted: return "SCD41_RECOVERY_RESTARTED";
        case EventCode::Scd41RecoveryFailed: return "SCD41_RECOVERY_FAILED";
        case EventCode::Scd41Recovered: return "SCD41_RECOVERED";
        case EventCode::Scd41DriverError: return "SCD41_DRIVER_ERROR";
        case EventCode::Scd41Stabilizing: return "SCD41_STABILIZING";
        case EventCode::Scd41FrcSucceeded: return "SCD41_FRC_SUCCEEDED";
        case EventCode::Scd41FrcFailed: return "SCD41_FRC_FAILED";
        case EventCode::Scd41FactoryResetSucceeded: return "SCD41_FACTORY_RESET_SUCCEEDED";
        case EventCode::Scd41FactoryResetFailed: return "SCD41_FACTORY_RESET_FAILED";
        case EventCode::FramRecordCorrupt: return "FRAM_RECORD_CORRUPT";
        case EventCode::FramCheckpointFailed: return "FRAM_CHECKPOINT_FAILED";
    }
    return "UNKNOWN";
}

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

// Management data is journaled independently from data slots.  The legacy
// superblock at 0x0000 remains untouched so a failed first migration never
// destroys the only usable copy.
constexpr uint32_t FRAM_CHECKPOINT_MAGIC = 0x43504B54; // "CPKT"
constexpr uint16_t FRAM_CHECKPOINT_VERSION = 1;

struct FramWalStats {
    uint32_t droppedRecords;
    uint16_t highWaterRecords;
};

struct FramCheckpoint {
    uint32_t magic;
    uint16_t checkpointVersion;
    uint32_t generation;
    FramSuperblock superblock;
    FramWalStats stats;
    uint16_t crc16;
    uint8_t committed;
};

constexpr uint32_t FRAM_SD_TRANSACTION_MAGIC = 0x53445458; // "SDTX"
constexpr uint16_t FRAM_SD_TRANSACTION_VERSION = 1;

struct FramSdTransaction {
    uint32_t magic;
    uint16_t transactionVersion;
    uint32_t generation;
    uint32_t sequence;
    uint16_t lineLength;
    uint16_t lineCrc16;
    char filename[48];
    uint8_t active;
    uint16_t crc16;
    uint8_t committed;
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
    VALID_BME690_TPH = 1u << 9,
    VALID_BME690_GAS = 1u << 10,
};

constexpr uint8_t SCD41_STATE_SHIFT = 24;
constexpr uint32_t SCD41_STATE_MASK = 0x7u << SCD41_STATE_SHIFT;

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

struct SensorRecordV5 {
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

    float bme690TemperatureC;
    float bme690HumidityRh;
    float bme690PressureHpa;
    float bme690GasResistanceOhm;
    uint8_t bme690GasIndex;
    uint8_t bme690Status;

    // UINT16_MAX means that no successful SCD41 sample has been observed.
    // Seconds are sufficient for diagnosing long stale intervals and keep v5
    // within its existing 128-byte FRAM slot.
    uint16_t co2AgeSeconds;
};

struct FramRecordHeader {
    uint32_t sequence;
    uint16_t length;
    uint16_t crc;
    uint8_t committed;
}; // 9 bytes

// 結合して FRAM に書き込む完全なレコード
struct PersistentRecordV5 {
    FramRecordHeader header;
    SensorRecordV5 data;
};

enum SensorValidFlagsV6 : uint16_t {
    V6_VALID_TEMP = 1u << 0,
    V6_VALID_HUMIDITY = 1u << 1,
    V6_VALID_PRESSURE = 1u << 2,
    V6_VALID_CO2 = 1u << 3,
    V6_VALID_VOC = 1u << 4,
    V6_VALID_NOX = 1u << 5,
    V6_VALID_HR = 1u << 6,
    V6_VALID_SPO2 = 1u << 7,
    V6_VALID_DISPLAY_ALTITUDE = 1u << 8,
    V6_VALID_RAW_ALTITUDE = 1u << 9,
    V6_VALID_SEA_LEVEL_PRESSURE = 1u << 10,
    V6_VALID_PRESSURE_OFFSET = 1u << 11,
    V6_VALID_BME690_TPH = 1u << 12,
    V6_VALID_BME690_GAS = 1u << 13,
    V6_VALID_SGP41_RAW = 1u << 14,
    V6_VALID_PPG_QUALITY = 1u << 15
};

// v6 uses fixed-point fields and packed state metadata to retain a 128-byte
// physical slot while adding the evidence needed to explain stale or stepped
// measurements. UINT16_MAX is the common sentinel for unknown ages.
struct SensorRecordV6 {
    uint32_t sequence;
    uint32_t uptimeMs;
    int64_t sampleMonotonicUs;
    int64_t utcEpochMs;

    int16_t temperatureCentiC;
    uint16_t humidityCentiRh;
    uint16_t pressureDeciHpa;
    uint16_t co2Ppm;
    int16_t vocIndex;
    int16_t noxIndex;
    uint16_t heartRateDeciBpm;
    uint16_t spo2CentiPercent;
    int16_t displayAltitudeDeciM;
    int16_t rawAltitudeDeciM;
    uint16_t seaLevelPressureDeciHpa;
    int16_t pressureOffsetCentiHpa;

    uint16_t seaLevelPressureAgeSeconds;
    uint16_t co2AgeSeconds;
    uint16_t sgp41AgeSeconds;
    uint16_t srawVoc;
    uint16_t srawNox;
    uint16_t sgp41CompensationRhTicks;
    uint16_t sgp41CompensationTemperatureTicks;

    int32_t gnssLatitudeE7;
    int32_t gnssLongitudeE7;
    int16_t gnssAltitudeDeciM;
    uint16_t gnssSpeedCentiMps;
    uint16_t gnssCourseDeciDeg;
    uint16_t gnssHdopCenti;
    uint16_t gnssAgeSeconds;
    uint16_t ppsAgeMs;
    uint8_t gnssSatellites;
    uint8_t gnssValidFlags;

    // bits 0..2 time source, 3..4 pressure state, 5..7 pressure source
    uint8_t sourceBits;
    // bits 0..2 device state, 3..5 error, 6..7 saturated consecutive errors
    uint8_t scd41Health;
    uint8_t sgp41Health;
    uint16_t scd41RawError;

    int16_t bme690TemperatureCentiC;
    uint16_t bme690HumidityCentiRh;
    uint16_t bme690PressureDeciHpa;
    uint32_t bme690GasResistanceOhm;
    uint8_t bme690GasIndex;
    uint8_t bme690Status;
    uint16_t bme690AgeSeconds;
    uint8_t bme690Health;
    uint16_t sht45AgeSeconds;
    uint8_t sht45Health;
    uint16_t bmp581AgeSeconds;
    uint8_t bmp581Health;

    uint16_t i2cLockTimeouts;
    uint16_t i2cCommunicationErrors;
    uint16_t droppedRecords;
    uint16_t validFlags;
    uint8_t formatTag;
};

struct PersistentRecordV6 {
    FramRecordHeader header;
    SensorRecordV6 data;
};

struct EventRecord {
    FramRecordHeader header;
    uint32_t uptimeMs;
    uint16_t eventCode;
    int32_t detail;
}; // 19 bytes

constexpr uint32_t FRAM_QUARANTINE_MAGIC = 0x51465231; // "QFR1"
constexpr uint16_t FRAM_QUARANTINE_VERSION = 1;

// Binary recovery evidence written to SD before a corrupt WAL tail is skipped.
// Parsers may scan for magic to recover later complete entries after a torn SD
// append. recordCrc16 covers every preceding byte in this structure.
struct FramQuarantineRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t slotIndex;
    uint32_t captureBootCount;
    uint32_t captureUptimeMs;
    uint16_t rawLength;
    uint16_t rawCrc16;
    uint8_t raw[RECORD_SLOT_SIZE];
    uint16_t recordCrc16;
};

#pragma pack(pop)

// 定数定義 (アドレスマップ)
constexpr uint16_t ADDR_SUPERBLOCK    = 0x0000;
constexpr uint16_t ADDR_CHECKPOINT_A  = 0x0040;
constexpr uint16_t ADDR_CHECKPOINT_B  = 0x00A0;
constexpr size_t CHECKPOINT_SLOT_SIZE = 0x0060;
constexpr uint16_t ADDR_STATE         = 0x0100;
constexpr uint16_t ADDR_SD_TX_A       = 0x0100;
constexpr uint16_t ADDR_SD_TX_B       = 0x0180;
constexpr size_t SD_TX_SLOT_SIZE      = 0x0080;
constexpr uint16_t ADDR_EVENT_JOURNAL = 0x0200;
constexpr uint16_t ADDR_RING_BUFFER   = 0x1000;

// 容量とレコードサイズの定義
constexpr size_t FRAM_CAPACITY        = 65536; // 64KB (32KB x 2)
constexpr size_t RING_BUFFER_SIZE     = FRAM_CAPACITY - ADDR_RING_BUFFER; // 61440 bytes
constexpr size_t MAX_RECORDS          = RING_BUFFER_SIZE / RECORD_SLOT_SIZE; // 480 records

constexpr size_t EVENT_SLOT_SIZE      = 32;
constexpr size_t EVENT_JOURNAL_SIZE   = ADDR_RING_BUFFER - ADDR_EVENT_JOURNAL;
constexpr size_t MAX_EVENT_RECORDS    = EVENT_JOURNAL_SIZE / EVENT_SLOT_SIZE;
constexpr uint16_t EVENT_PAYLOAD_SIZE = sizeof(EventRecord) - sizeof(FramRecordHeader);

// v5 records written before CO2 quality metadata were 117 bytes long. The
// header length and CRC make both layouts distinguishable without a format bump.
constexpr uint16_t LEGACY_SENSOR_RECORD_V5_SIZE = offsetof(SensorRecordV5, co2AgeSeconds);

static_assert(sizeof(PersistentRecordV5) <= RECORD_SLOT_SIZE, "PersistentRecordV5 exceeds RECORD_SLOT_SIZE");
static_assert(sizeof(SensorRecordV6) == 119, "Unexpected SensorRecordV6 size");
static_assert(sizeof(PersistentRecordV6) == RECORD_SLOT_SIZE,
              "PersistentRecordV6 should fully occupy its slot");
static_assert(sizeof(FramQuarantineRecord) == 150, "Unexpected quarantine record size");
static_assert(sizeof(PersistentRecordV5) == RECORD_SLOT_SIZE, "PersistentRecordV5 should fully occupy its slot");
static_assert(sizeof(EventRecord) <= EVENT_SLOT_SIZE, "EventRecord exceeds EVENT_SLOT_SIZE");
static_assert(sizeof(FramCheckpoint) <= CHECKPOINT_SLOT_SIZE, "FramCheckpoint exceeds its slot");
static_assert(ADDR_CHECKPOINT_B + CHECKPOINT_SLOT_SIZE <= ADDR_STATE,
              "Checkpoint slots overlap persistent state");
static_assert(sizeof(FramSdTransaction) <= SD_TX_SLOT_SIZE,
              "FramSdTransaction exceeds its slot");
static_assert(ADDR_SD_TX_B + SD_TX_SLOT_SIZE <= ADDR_EVENT_JOURNAL,
              "SD transaction slots overlap event journal");

} // namespace storage
