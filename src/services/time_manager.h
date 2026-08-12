#pragma once

namespace services {

class TimeManager {
public:
    // Wi-Fiに接続し、NTPサーバーと同期後、Wi-Fiを切断する
    static void begin();
};

} // namespace services
