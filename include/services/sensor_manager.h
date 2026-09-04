#pragma once

#include "core/sensor_snapshot.h"
#include "drivers/sensors/sht45_sensor.h"
#include "drivers/sensors/bmp581_sensor.h"
#include "drivers/sensors/scd41_sensor.h"
#include "drivers/sensors/sgp41_sensor.h"
#include "drivers/sensors/max30102_sensor.h"
#include "drivers/sensors/lc76g_sensor.h"
#include "drivers/sensors/bme690_sensor.h"
#include "services/gnss_time_sync_service.h"
#include "storage/storage_manager.h"
#include <cstdint>
#include <freertos/FreeRTOS.h>

namespace services {

class SensorManager {
public:
    bool begin(storage::StorageManager& storageManager);
    void update(uint32_t nowMs);

    core::SensorSnapshot snapshot() const;
    core::SystemStatus status() const;
    
    // Calibration & Maintenance
    void setSeaLevelPressure(float hpa, core::PressureFieldState state = core::PressureFieldState::Valid);
    bool calibrateScd41(uint16_t referencePpm, drivers::sensors::Scd41FrcResult& result);
    bool factoryResetScd41();
    bool triggerSht45Heater();
    bool startBmp581Calibration(float referenceAltitudeM);

    GnssTimeSyncService* getGnssTimeSyncService() { return &gnssTimeSync_; }

private:
    void trackScd41Health(uint32_t nowMs);
    void publishSnapshot();

    storage::StorageManager* storage_ = nullptr;
    core::SensorSnapshot snapshot_ {};
    core::SystemStatus status_ {};
    core::SensorSnapshot publishedSnapshot_ {};
    core::SystemStatus publishedStatus_ {};
    mutable portMUX_TYPE publicationMux_ = portMUX_INITIALIZER_UNLOCKED;
    
    uint32_t highHumidityStartMs_ = 0;
    uint32_t lastHeaterRunMs_ = 0;
    
    float slpEma_ = NAN;
    uint32_t lastAmedasUpdateMs_ = 0;

    drivers::sensors::Scd41Condition lastScd41Condition_ {
        drivers::sensors::Scd41Condition::AwaitingFirstSample};
    bool scd41ConditionInitialized_ {false};
    bool scd41FaultActive_ {false};
    
    drivers::sensors::Sht45Sensor sht45_;
    drivers::sensors::Bmp581Sensor bmp581_;
    drivers::sensors::Scd41Sensor scd41_;
    drivers::sensors::Sgp41Sensor sgp41_;
    drivers::sensors::Max30102Sensor max30102_;
    drivers::sensors::Lc76gSensor lc76g_{Serial1};
    drivers::sensors::Bme690Sensor bme690_;
    GnssTimeSyncService gnssTimeSync_;
};

} // namespace services
