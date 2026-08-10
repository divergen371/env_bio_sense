#pragma once
#include "drivers/sensors/bmp5_sensor_base.h"

namespace drivers {
namespace sensors {

class Bmp581Sensor : public Bmp5SensorBase {
public:
    Bmp581Sensor() = default;
    virtual ~Bmp581Sensor() = default;

    core::SensorId id() const override { return core::SensorId::Bmp581; }

protected:
    // Adafruit STEMMA QT BMP581 defaults to 0x47. 
    // If it's 0x46, change it here or make it configurable.
    uint8_t getI2cAddress() const override { return 0x47; } 
    uint8_t getExpectedChipId() const override { return BMP5_CHIP_ID_PRIM; }
    const char* getSensorName() const override { return "BMP581"; }
};

} // namespace sensors
} // namespace drivers
