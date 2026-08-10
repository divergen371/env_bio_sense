#pragma once

#include "core/sensor_types.h"
#include "core/sensor_snapshot.h"
#include <FS.h>
#include <SD.h>

namespace services {

class DataLogger {
public:
    DataLogger();

    bool begin();
    void logSnapshot(const core::SensorSnapshot& snapshot, uint32_t uptimeMs);
    bool isAvailable() const { return available_; }

private:
    bool available_ {false};
    String currentFilename_ {""};

    bool createNewFile();
    void writeHeader(File& file);
};

} // namespace services
