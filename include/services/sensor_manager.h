#pragma once

#include "core/sensor_snapshot.h"
#include "drivers/sensors/sht45_sensor.h"
#include "drivers/sensors/bmp581_sensor.h"
#include "drivers/sensors/scd41_sensor.h"
#include "drivers/sensors/sgp41_sensor.h"
#include "drivers/sensors/max30102_sensor.h"
#include "storage/storage_manager.h"
#include <cstdint>

namespace services {

class SensorManager {
public:
    bool begin(storage::StorageManager& storageManager);
    void update(uint32_t nowMs);

    const core::SensorSnapshot& snapshot() const { return snapshot_; }
    const core::SystemStatus& status() const { return status_; }
    
    // システム操作
    void setSeaLevelPressure(float hpa, core::PressureFieldState state = core::PressureFieldState::Valid);
    bool calibrateScd41(uint16_t targetPpm, uint16_t& frcCorrection);
    bool triggerSht45Heater();
    bool isScd41CalibrationRecommended() const;
    bool startBmp581Calibration(float referenceAltitudeM);

private:
    storage::StorageManager* storage_ = nullptr;
    core::SensorSnapshot snapshot_ {};
    core::SystemStatus status_ {};
    
    uint32_t highHumidityStartMs_ = 0;
    uint32_t lastHeaterRunMs_ = 0;
    
    drivers::sensors::Sht45Sensor sht45_;
    drivers::sensors::Bmp581Sensor bmp581_;
    drivers::sensors::Scd41Sensor scd41_;
    drivers::sensors::Sgp41Sensor sgp41_;
    drivers::sensors::Max30102Sensor max30102_;
};

} // namespace services
