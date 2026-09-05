#pragma once

#include <cstdint>
#include <WString.h>
#include "services/sensor_manager.h"
#include "services/wifi_manager.h"
#include "services/location_service.h"
#include "storage/storage_manager.h"
#include <freertos/FreeRTOS.h>

namespace services {

struct AmedasStationStatus {
    char id[6] {};
    float distanceKm {};
    float seaLevelPressureHpa {};
    uint8_t qualityCode {0xFFu};
    bool used {false};
};

struct AmedasStatus {
    core::PressureFieldState state {core::PressureFieldState::Invalid};
    core::DeviceLocation location {};
    float interpolatedSeaLevelPressureHpa {};
    uint32_t observationAgeMs {UINT32_MAX};
    int64_t observationUtcMs {};
    float minDistanceKm {};
    float maxDistanceKm {};
    uint8_t cachedStations {};
    uint8_t usedStations {};
    bool lastFetchSucceeded {false};
    AmedasStationStatus stations[5] {};
};

class WeatherService {
public:
    WeatherService(SensorManager& sensorManager, WifiManager& wifiManager,
                   LocationService& locationService,
                   storage::StorageManager& storageManager);
    
    void begin();
    void update(uint32_t nowMs);
    void forceUpdate(float pressureHpa); // スマホ等からPOSTされた場合用
    AmedasStatus status(uint32_t nowMs) const;

private:
    SensorManager& sensorManager_;
    WifiManager& wifiManager_;
    LocationService& locationService_;
    storage::StorageManager& storageManager_;
    
    uint32_t nextFetchMs_ = 0;
    
    // JMA AMeDAS 用の追加メンバ
    struct AmedasStation {
        char id[6] {};
        float distanceKm {};
    };
    AmedasStation nearestStations_[5];
    int numCachedStations_ = 0;
    double stationSelectionLatDeg_ {};
    double stationSelectionLonDeg_ {};
    core::LocationSource stationSelectionSource_ {
        core::LocationSource::Unavailable};
    bool hasStationSelectionLocation_ {false};

    mutable portMUX_TYPE statusMux_ = portMUX_INITIALIZER_UNLOCKED;
    AmedasStatus status_ {};
    uint32_t lastGoodObservationCapturedMs_ {};
    bool hasLastGoodField_ {false};

    bool fetchNearestStations(const core::DeviceLocation& location);
    bool fetchLatestTime(String& mapTime, int64_t& observationUtcMs);
    bool fetchSeaLevelPressure(const core::DeviceLocation& location,
                               uint32_t nowMs);
    void refreshPressureFieldState(uint32_t nowMs);
    void publishStatus(const AmedasStatus& status);
    void persistPressureFieldAttempt(
        const AmedasStatus& attempt, float previousPressureHpa,
        core::PressureFieldState resultingState, const char* result);
};

} // namespace services
