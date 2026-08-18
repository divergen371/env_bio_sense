#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>

namespace drivers {
namespace sensors {

struct PpsEvent {
    uint32_t sequence {};
    int64_t monotonicUs {};
};

class IGnssSensor : public ISensor {
public:
    virtual bool readGnss(core::GnssData& out, uint32_t nowMs) const = 0;
    virtual core::GnssStatus status(uint32_t nowMs) const = 0;
    virtual bool takePpsEvent(PpsEvent& out) = 0;
};

class Lc76gSensor : public IGnssSensor {
public:
    explicit Lc76gSensor(HardwareSerial& serial);

    // ISensor
    core::SensorId id() const override { return core::SensorId::Gnss; }
    bool begin() override;
    void update(uint32_t nowMs) override;
    core::DeviceState state() const override { return status_.transportState; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }

    // IGnssSensor
    bool readGnss(core::GnssData& out, uint32_t nowMs) const override;
    core::GnssStatus status(uint32_t nowMs) const override;
    bool takePpsEvent(PpsEvent& out) override;

    // ISR
    static void IRAM_ATTR onPps();

private:
    HardwareSerial& serial_;
    TinyGPSPlus gps_;

    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};

    core::GnssData data_ {};
    core::GnssStatus status_ {};

    // 内部状態
    uint32_t lastValidNmeaMs_ {0};
    uint32_t baudRate_ {0};
    bool isBaudProbing_ {false};
    uint32_t baudProbeStartMs_ {0};
    uint8_t probeStep_ {0};

    void processNmea(uint32_t nowMs);
    void updateFixStatus(uint32_t nowMs);
    void updateState(uint32_t nowMs);
    void probeBaudRate(uint32_t nowMs);

    // PPS用のstatic変数はcpp側で定義
};

} // namespace sensors
} // namespace drivers
