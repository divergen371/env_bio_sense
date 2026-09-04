#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace hal {

enum class I2cDevice : uint8_t {
    Unknown,
    Sht45,
    Bmp581,
    Scd41,
    Sgp41,
    Max30102,
    Bme690,
    Fram,
    Oled,
    Count
};

enum class I2cOperation : uint8_t {
    Init,
    Read,
    Write,
    Measure,
    Maintenance,
    Count
};

struct I2cDiagnosticCounters {
    uint32_t lockTimeouts {};
    uint32_t communicationErrors {};
};

class I2cBus {
public:
    static bool begin();
    static void scan();

    // 手動での呼び出しは推奨しません。I2cLockGuard を使用してください。
    static bool lock(uint32_t timeoutMs = 100);
    static void unlock();
    static void noteLockTimeout(I2cDevice device, I2cOperation operation);
    static void noteCommunicationError(I2cDevice device, I2cOperation operation);
    static I2cDiagnosticCounters diagnostics(I2cDevice device,
                                             I2cOperation operation);
    static I2cDiagnosticCounters diagnosticTotals();

private:
    static SemaphoreHandle_t mutex_;
    static portMUX_TYPE diagnosticsMux_;
    static I2cDiagnosticCounters diagnostics_[
        static_cast<uint8_t>(I2cDevice::Count)][
        static_cast<uint8_t>(I2cOperation::Count)];
};

// RAII パターンのロックガード
class I2cLockGuard {
public:
    explicit I2cLockGuard(uint32_t timeoutMs = 100);
    I2cLockGuard(I2cDevice device, I2cOperation operation,
                 uint32_t timeoutMs = 100);
    ~I2cLockGuard();

    // コピーとムーブを禁止
    I2cLockGuard(const I2cLockGuard&) = delete;
    I2cLockGuard& operator=(const I2cLockGuard&) = delete;

    bool acquired() const { return acquired_; }

private:
    bool acquired_;
};

} // namespace hal
