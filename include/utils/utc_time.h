#pragma once

#include <cstdint>
#include <limits>

namespace utils {

inline bool isLeapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

inline uint8_t daysInMonth(int year, uint8_t month) {
    static constexpr uint8_t DAYS[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12) return 0;
    return month == 2 && isLeapYear(year) ? 29 : DAYS[month - 1];
}

// Gregorian civil date to days since 1970-01-01. This is deliberately
// independent of process timezone and libc TZ configuration.
inline int64_t daysFromCivil(int year, uint8_t month, uint8_t day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned shiftedMonth = static_cast<unsigned>(
        static_cast<int>(month) + (month > 2 ? -3 : 9));
    const unsigned dayOfYear = (153 * shiftedMonth + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 -
        yearOfEra / 100 + dayOfYear;
    return static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
}

inline bool utcEpochMsFromCalendar(int year, uint8_t month, uint8_t day,
                                   uint8_t hour, uint8_t minute,
                                   uint8_t second, uint16_t millisecond,
                                   int64_t& epochMs) {
    if (year < 1970 || year > 9999 || month < 1 || month > 12 ||
        day < 1 || day > daysInMonth(year, month) || hour > 23 ||
        minute > 59 || second > 60 || millisecond > 999) {
        return false;
    }

    // Treat the rare leap-second representation as the following second.
    const int64_t seconds = daysFromCivil(year, month, day) * 86400LL +
        static_cast<int64_t>(hour) * 3600LL +
        static_cast<int64_t>(minute) * 60LL + second;
    epochMs = seconds * 1000LL + millisecond;
    return true;
}

inline bool epochSecondsToMicroseconds(int64_t epochSeconds, int64_t& epochUs) {
    constexpr int64_t SCALE = 1000000LL;
    if (epochSeconds > std::numeric_limits<int64_t>::max() / SCALE ||
        epochSeconds < std::numeric_limits<int64_t>::min() / SCALE) {
        return false;
    }
    epochUs = epochSeconds * SCALE;
    return true;
}

inline uint32_t monotonicAgeMs(int64_t nowUs, int64_t eventUs) {
    if (eventUs <= 0 || nowUs < eventUs) return UINT32_MAX;
    const uint64_t ageMs = static_cast<uint64_t>(nowUs - eventUs) / 1000ULL;
    return ageMs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ageMs);
}

} // namespace utils
