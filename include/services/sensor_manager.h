#pragma once

#include "core/sensor_snapshot.h"
#include "drivers/sensors/sht45_sensor.h"
#include "drivers/sensors/bmp581_sensor.h"
#include "drivers/sensors/scd41_sensor.h"
#include "drivers/sensors/sgp41_sensor.h"
#include "drivers/sensors/max30102_sensor.h"
#include "drivers/sensors/lc76g_sensor.h"
#include "services/gnss_time_sync_service.h"
#include "storage/storage_manager.h"
#include <cstdint>

namespace services {

class SensorManager {
public:
    bool begin(storage::StorageManager& storageManager);
    void update(uint32_t nowMs);

    const core::SensorSnapshot& snapshot() const { return snapshot_; }
    const core::SystemStatus& status() const { return status_; }
    
    // Calibration & Maintenance
    void setSeaLevelPressure(float hpa, core::PressureFieldState state = core::PressureFieldState::Valid);
    bool calibrateScd41(uint16_t targetPpm, uint16_t& frcCorrection);
    bool isScd41CalibrationRecommended() const;
    bool triggerSht45Heater();
    bool startBmp581Calibration(float referenceAltitudeM);

    GnssTimeSyncService* getGnssTimeSyncService() { return &gnssTimeSync_; }

private:
    storage::StorageManager* storage_ = nullptr;
    core::SensorSnapshot snapshot_ {};
    core::SystemStatus status_ {};
    
    uint32_t highHumidityStartMs_ = 0;
    uint32_t lastHeaterRunMs_ = 0;
    
    float slpEma_ = NAN;
    uint32_t lastAmedasUpdateMs_ = 0;
    
    drivers::sensors::Sht45Sensor sht45_;
    drivers::sensors::Bmp581Sensor bmp581_;
    drivers::sensors::Scd41Sensor scd41_;
    drivers::sensors::Sgp41Sensor sgp41_;
    drivers::sensors::Max30102Sensor max30102_;
    drivers::sensors::Lc76gSensor lc76g_{Serial1};
    GnssTimeSyncService gnssTimeSync_;
};

} // namespace services
