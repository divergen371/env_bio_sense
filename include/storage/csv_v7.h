#pragma once

#include "storage/storage_record_codec.h"
#include "storage/storage_records.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace storage {

constexpr size_t CSV_V7_COLUMN_COUNT = 71;
constexpr size_t MAX_CSV_LINE_LENGTH = 768;
constexpr const char* CSV_V7_HEADER =
    "Sequence,UptimeMs,SampleMonotonicUs,TimestampUtc,TimeSource,CO2_ppm,Temp_C,RH_pct,Pressure_hPa,VOC_Index,NOx_Index,HR_bpm,SpO2_pct,BMP_DisplayAltitude_m,BMP_RawAltitude_m,SeaLevelPressure_hPa,PressureReferenceState,PressureReferenceSource,PressureReferenceAgeMs,BMP_PressureOffset_hPa,GNSS_Lat_deg,GNSS_Lon_deg,GNSS_AltMSL_m,GNSS_Speed_mps,GNSS_Course_deg,GNSS_Satellites,GNSS_HDOP,GNSS_FixValid,GNSS_TimeValid,GNSS_AgeMs,PPS_AgeMs,GNSS_TimeDisciplined,ValidFlags,BME690_Temp_C,BME690_RH_pct,BME690_Pressure_hPa,BME690_GasResistance_Ohm,BME690_GasValid,BME690_HeaterStable,BME690_GasIndex,BME690_StatusHex,BME690_AgeMs,BME690_State,BME690_Error,BME690_ConsecutiveErrors,CO2_Valid,CO2_AgeMs,SCD41_State,SCD41_Error,SCD41_ConsecutiveErrors,SCD41_RawError,SGP41_Valid,SGP41_AgeMs,SGP41_State,SGP41_Error,SGP41_ConsecutiveErrors,SRAW_VOC,SRAW_NOX,SGP41_CompensationRH_pct,SGP41_CompensationTemp_C,SHT45_AgeMs,SHT45_State,SHT45_Error,SHT45_ConsecutiveErrors,BMP581_AgeMs,BMP581_State,BMP581_Error,BMP581_ConsecutiveErrors,I2C_LockTimeouts,I2C_CommunicationErrors,FRAM_DroppedRecords";

namespace csv_v7_detail {

class Builder {
public:
    Builder(char* buffer, size_t capacity)
        : buffer_(buffer), capacity_(capacity) {
        if (buffer_ != nullptr && capacity_ > 0) buffer_[0] = '\0';
    }

    void add(const char* value) {
        if (fieldCount_ > 0) append(",");
        append(value == nullptr ? "" : value);
        ++fieldCount_;
    }

    void addFormat(const char* format, ...) {
        char value[48] {};
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(value, sizeof(value), format, args);
        va_end(args);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(value)) {
            ok_ = false;
            add("");
            return;
        }
        add(value);
    }

    bool finish(size_t expectedFields) {
        if (buffer_ == nullptr || capacity_ == 0 || !ok_ ||
            fieldCount_ != expectedFields || length_ >= capacity_) {
            if (buffer_ != nullptr && capacity_ > 0) buffer_[0] = '\0';
            return false;
        }
        buffer_[length_] = '\0';
        return true;
    }

private:
    void append(const char* text) {
        const size_t textLength = strlen(text);
        if (buffer_ == nullptr || capacity_ == 0 ||
            textLength > capacity_ - 1 || length_ > capacity_ - 1 - textLength) {
            ok_ = false;
            return;
        }
        memcpy(buffer_ + length_, text, textLength);
        length_ += textLength;
        buffer_[length_] = '\0';
    }

    char* buffer_ {nullptr};
    size_t capacity_ {};
    size_t length_ {};
    size_t fieldCount_ {};
    bool ok_ {true};
};

inline const char* timeSourceName(core::TimeSource source) {
    switch (source) {
        case core::TimeSource::Manual: return "MANUAL";
        case core::TimeSource::Ntp: return "NTP";
        case core::TimeSource::Gnss: return "GNSS";
        case core::TimeSource::Holdover: return "HOLDOVER";
        default: return "UNSET";
    }
}

inline const char* pressureStateName(core::PressureFieldState state) {
    switch (state) {
        case core::PressureFieldState::Valid: return "VALID";
        case core::PressureFieldState::LastKnown: return "LAST_KNOWN";
        case core::PressureFieldState::StaticFallback: return "STATIC_FALLBACK";
        default: return "INVALID";
    }
}

inline const char* pressureSourceName(core::PressureReferenceSource source) {
    switch (source) {
        case core::PressureReferenceSource::Amedas: return "AMEDAS";
        case core::PressureReferenceSource::Gnss: return "GNSS";
        case core::PressureReferenceSource::Manual: return "MANUAL";
        case core::PressureReferenceSource::Stored: return "STORED";
        default: return "UNSET";
    }
}

inline const char* deviceStateName(core::DeviceState state) {
    switch (state) {
        case core::DeviceState::Initializing: return "INITIALIZING";
        case core::DeviceState::Ready: return "READY";
        case core::DeviceState::Degraded: return "DEGRADED";
        case core::DeviceState::Warning: return "WARNING";
        case core::DeviceState::Offline: return "OFFLINE";
        case core::DeviceState::RetryWait: return "RETRY_WAIT";
        case core::DeviceState::Error: return "ERROR";
        default: return "UNKNOWN";
    }
}

inline const char* errorName(core::ErrorCode error) {
    switch (error) {
        case core::ErrorCode::None: return "NONE";
        case core::ErrorCode::NotFound: return "NOT_FOUND";
        case core::ErrorCode::InitFailed: return "INIT_FAILED";
        case core::ErrorCode::ReadFailed: return "READ_FAILED";
        case core::ErrorCode::Timeout: return "TIMEOUT";
        case core::ErrorCode::InvalidData: return "INVALID_DATA";
        case core::ErrorCode::BusError: return "BUS_ERROR";
        case core::ErrorCode::Unsupported: return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

inline void addAgeMs(Builder& builder, uint16_t seconds) {
    if (seconds == UINT16_MAX) builder.add("");
    else builder.addFormat("%lu", static_cast<unsigned long>(seconds) * 1000ul);
}

inline void addHealth(Builder& builder, uint8_t packed) {
    builder.add(deviceStateName(codec::healthState(packed)));
    builder.add(errorName(codec::healthError(packed)));
    builder.addFormat("%u", codec::healthConsecutiveErrors(packed));
}

} // namespace csv_v7_detail

inline bool formatCsvLineV7(char* buffer, size_t size,
                            const SensorRecordV6& rec) {
    using namespace csv_v7_detail;
    Builder b(buffer, size);
    const uint16_t f = rec.validFlags;
    const bool utcValid = (rec.gnssValidFlags & GNSS_VALID_UTC) != 0;

    char timestamp[32] {};
    if (utcValid) {
        const time_t epoch = static_cast<time_t>(rec.utcEpochMs / 1000);
        const uint16_t milliseconds = static_cast<uint16_t>(rec.utcEpochMs % 1000);
        struct tm value {};
        if (gmtime_r(&epoch, &value) != nullptr) {
            snprintf(timestamp, sizeof(timestamp),
                     "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                     value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                     value.tm_hour, value.tm_min, value.tm_sec, milliseconds);
        }
    }

    b.addFormat("%lu", static_cast<unsigned long>(rec.sequence));
    b.addFormat("%lu", static_cast<unsigned long>(rec.uptimeMs));
    b.addFormat("%lld", static_cast<long long>(rec.sampleMonotonicUs));
    b.add(timestamp);
    b.add(timeSourceName(codec::timeSource(rec.sourceBits)));
    if (f & V6_VALID_CO2) b.addFormat("%u", rec.co2Ppm); else b.add("");
    if (f & V6_VALID_TEMP) b.addFormat("%.2f", rec.temperatureCentiC / 100.0f); else b.add("");
    if (f & V6_VALID_HUMIDITY) b.addFormat("%.2f", rec.humidityCentiRh / 100.0f); else b.add("");
    if (f & V6_VALID_PRESSURE) b.addFormat("%.1f", rec.pressureDeciHpa / 10.0f); else b.add("");
    if (f & V6_VALID_VOC) b.addFormat("%d", rec.vocIndex); else b.add("");
    if (f & V6_VALID_NOX) b.addFormat("%d", rec.noxIndex); else b.add("");
    if (f & V6_VALID_HR) b.addFormat("%.1f", rec.heartRateDeciBpm / 10.0f); else b.add("");
    if (f & V6_VALID_SPO2) b.addFormat("%.2f", rec.spo2CentiPercent / 100.0f); else b.add("");
    if (f & V6_VALID_DISPLAY_ALTITUDE) b.addFormat("%.1f", rec.displayAltitudeDeciM / 10.0f); else b.add("");
    if (f & V6_VALID_RAW_ALTITUDE) b.addFormat("%.1f", rec.rawAltitudeDeciM / 10.0f); else b.add("");
    if (f & V6_VALID_SEA_LEVEL_PRESSURE) b.addFormat("%.1f", rec.seaLevelPressureDeciHpa / 10.0f); else b.add("");
    b.add(pressureStateName(codec::pressureState(rec.sourceBits)));
    b.add(pressureSourceName(codec::pressureSource(rec.sourceBits)));
    addAgeMs(b, rec.seaLevelPressureAgeSeconds);
    if (f & V6_VALID_PRESSURE_OFFSET) b.addFormat("%.2f", rec.pressureOffsetCentiHpa / 100.0f); else b.add("");

    if (rec.gnssValidFlags & GNSS_VALID_FIX) b.addFormat("%.7f", rec.gnssLatitudeE7 / 1e7); else b.add("");
    if (rec.gnssValidFlags & GNSS_VALID_FIX) b.addFormat("%.7f", rec.gnssLongitudeE7 / 1e7); else b.add("");
    if (rec.gnssValidFlags & GNSS_VALID_ALTITUDE) b.addFormat("%.1f", rec.gnssAltitudeDeciM / 10.0f); else b.add("");
    if (rec.gnssValidFlags & GNSS_VALID_SPEED) b.addFormat("%.2f", rec.gnssSpeedCentiMps / 100.0f); else b.add("");
    if (rec.gnssValidFlags & GNSS_VALID_COURSE) b.addFormat("%.1f", rec.gnssCourseDeciDeg / 10.0f); else b.add("");
    b.addFormat("%u", rec.gnssSatellites);
    if (rec.gnssValidFlags & GNSS_VALID_HDOP) b.addFormat("%.2f", rec.gnssHdopCenti / 100.0f); else b.add("");
    b.addFormat("%u", (rec.gnssValidFlags & GNSS_VALID_FIX) ? 1 : 0);
    b.addFormat("%u", utcValid ? 1 : 0);
    addAgeMs(b, rec.gnssAgeSeconds);
    if (rec.ppsAgeMs == UINT16_MAX) b.add(""); else b.addFormat("%u", rec.ppsAgeMs);
    b.addFormat("%u", (rec.gnssValidFlags & GNSS_TIME_DISCIPLINED) ? 1 : 0);
    b.addFormat("0x%04X", rec.validFlags);

    if (f & V6_VALID_BME690_TPH) b.addFormat("%.2f", rec.bme690TemperatureCentiC / 100.0f); else b.add("");
    if (f & V6_VALID_BME690_TPH) b.addFormat("%.2f", rec.bme690HumidityCentiRh / 100.0f); else b.add("");
    if (f & V6_VALID_BME690_TPH) b.addFormat("%.1f", rec.bme690PressureDeciHpa / 10.0f); else b.add("");
    if (f & V6_VALID_BME690_GAS) b.addFormat("%lu", static_cast<unsigned long>(rec.bme690GasResistanceOhm)); else b.add("");
    b.addFormat("%u", (f & V6_VALID_BME690_GAS) ? 1 : 0);
    b.addFormat("%u", (rec.bme690Status & 0x10u) ? 1 : 0);
    b.addFormat("%u", rec.bme690GasIndex);
    b.addFormat("0x%02X", rec.bme690Status);
    addAgeMs(b, rec.bme690AgeSeconds);
    addHealth(b, rec.bme690Health);

    b.addFormat("%u", (f & V6_VALID_CO2) ? 1 : 0);
    addAgeMs(b, rec.co2AgeSeconds);
    addHealth(b, rec.scd41Health);
    b.addFormat("%u", rec.scd41RawError);

    b.addFormat("%u", (f & V6_VALID_VOC) ? 1 : 0);
    addAgeMs(b, rec.sgp41AgeSeconds);
    addHealth(b, rec.sgp41Health);
    if (f & V6_VALID_SGP41_RAW) b.addFormat("%u", rec.srawVoc); else b.add("");
    if (f & V6_VALID_SGP41_RAW) b.addFormat("%u", rec.srawNox); else b.add("");
    if (f & V6_VALID_SGP41_RAW) b.addFormat("%.2f", rec.sgp41CompensationRhTicks * 100.0f / 65535.0f); else b.add("");
    if (f & V6_VALID_SGP41_RAW) b.addFormat("%.2f", rec.sgp41CompensationTemperatureTicks * 175.0f / 65535.0f - 45.0f); else b.add("");

    addAgeMs(b, rec.sht45AgeSeconds);
    addHealth(b, rec.sht45Health);
    addAgeMs(b, rec.bmp581AgeSeconds);
    addHealth(b, rec.bmp581Health);
    b.addFormat("%u", rec.i2cLockTimeouts);
    b.addFormat("%u", rec.i2cCommunicationErrors);
    b.addFormat("%u", rec.droppedRecords);
    return b.finish(CSV_V7_COLUMN_COUNT);
}

} // namespace storage
