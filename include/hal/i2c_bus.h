#pragma once

#include <cstdint>

namespace hal {

class I2cBus {
public:
    static bool begin();
    static void scan();
};

} // namespace hal
