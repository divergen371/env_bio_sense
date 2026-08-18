#pragma once

#include <Arduino.h>
#include <time.h>
#include "core/sensor_types.h"

namespace hal {

class Clock {
public:
    static int64_t nowMonotonicUs();
    
    // アンカー設定
    static void setUtcAnchor(int64_t utcEpochUs, int64_t monotonicUs, core::TimeSource source);
    
    // 時刻変換
    static int64_t utcEpochUsAt(int64_t monotonicUs);
    static int64_t utcEpochMsAt(int64_t monotonicUs);
    
    // 状態取得
    static core::TimeSource source();
    static bool isTimeSet();
    static bool isDisciplined();
    static void markTimeSet(); // レガシー互換用
    
    // POSIX互換
    static void setEpoch(time_t epoch);
    static time_t getEpoch();
    
    // フォーマット (JSTで表示する)
    // Returns YYYYMMDD (e.g. "20231024")
    static String getFormattedDate();
    
    // Returns HH:MM:SS (e.g. "14:30:00")
    static String getFormattedTime();

private:
    static portMUX_TYPE mux_;
    static int64_t anchorUtcUs_;
    static int64_t anchorMonotonicUs_;
    static core::TimeSource source_;
    static bool timeSet_;
};

} // namespace hal
