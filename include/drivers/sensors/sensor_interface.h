#pragma once

#include "core/sensor_types.h"

namespace drivers {
namespace sensors {

class ISensor {
public:
    virtual ~ISensor() = default;

    virtual core::SensorId id() const = 0;
    virtual bool begin() = 0;
    virtual void update(uint32_t nowMs) = 0;

    virtual core::DeviceState state() const = 0;
    virtual core::ErrorCode lastError() const = 0;
    virtual uint32_t lastSuccessMs() const = 0;
};

class IEnvironmentSensor : public ISensor {
public:
    virtual bool readEnvironment(core::EnvironmentData& out) const = 0;
};

} // namespace sensors
} // namespace drivers
