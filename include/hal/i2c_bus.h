#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace hal {

class I2cBus {
public:
    static bool begin();
    static void scan();

    // 手動での呼び出しは推奨しません。I2cLockGuard を使用してください。
    static bool lock(uint32_t timeoutMs = 100);
    static void unlock();

private:
    static SemaphoreHandle_t mutex_;
};

// RAII パターンのロックガード
class I2cLockGuard {
public:
    explicit I2cLockGuard(uint32_t timeoutMs = 100);
    ~I2cLockGuard();

    // コピーとムーブを禁止
    I2cLockGuard(const I2cLockGuard&) = delete;
    I2cLockGuard& operator=(const I2cLockGuard&) = delete;

    bool acquired() const { return acquired_; }

private:
    bool acquired_;
};

} // namespace hal
