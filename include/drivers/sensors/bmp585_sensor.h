#pragma once
#include "drivers/sensors/bmp5_sensor_base.h"

namespace drivers {
namespace sensors {

class Bmp585Sensor : public Bmp5SensorBase {
public:
    Bmp585Sensor() = default;
    virtual ~Bmp585Sensor() = default;

protected:
    uint8_t getI2cAddress() const override { return 0x46; } // BMP585 on custom board
    uint8_t getExpectedChipId() const override { return BMP5_CHIP_ID_SEC; }
    const char* getSensorName() const override { return "BMP585"; }
};

} // namespace sensors
} // namespace drivers
