#include "services/data_logger.h"
#include "services/logger.h"
#include "hal/pins.h"
#include <SPI.h>

namespace services {

DataLogger::DataLogger() {}

bool DataLogger::begin() {
    Logger::info("DataLogger", "Initializing SD Card...");
    
    // XIAO ESP32S3でSDモジュールを安定稼働させるため、
    // 明示的にSPIピン(SCK, MISO, MOSI)とCSを指定して初期化します。
    SPI.begin(SCK, MISO, MOSI, hal::pins::SD_CS);
    
    // Mount SD card (周波数を落として安定化させる)
    if (!SD.begin(hal::pins::SD_CS, SPI, 4000000)) {
        Logger::error("DataLogger", "SD Card Mount Failed. Make sure it is inserted.");
        available_ = false;
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Logger::error("DataLogger", "No SD card attached");
        available_ = false;
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Logger::info("DataLogger", "SD Card Type: %d, Size: %llu MB", cardType, cardSize);

    available_ = createNewFile();
    return available_;
}

bool DataLogger::createNewFile() {
    // Find a new filename to avoid overwriting (e.g. log_000.csv to log_999.csv)
    for (int i = 0; i < 1000; i++) {
        char filename[32];
        snprintf(filename, sizeof(filename), "/log_%03d.csv", i);
        if (!SD.exists(filename)) {
            currentFilename_ = filename;
            File file = SD.open(currentFilename_.c_str(), FILE_WRITE);
            if (!file) {
                Logger::error("DataLogger", "Failed to create file: %s", currentFilename_.c_str());
                return false;
            }
            writeHeader(file);
            file.close();
            Logger::info("DataLogger", "Created new log file: %s", currentFilename_.c_str());
            return true;
        }
    }
    Logger::error("DataLogger", "Log file limit reached (0-999)");
    return false;
}

void DataLogger::writeHeader(File& file) {
    file.println("UptimeMs,CO2_ppm,Temp_C,RH_pct,Pressure_hPa,VOC_Index,NOx_Index,HR_bpm,SpO2_pct");
}

void DataLogger::logSnapshot(const core::SensorSnapshot& snapshot, uint32_t uptimeMs) {
    if (!available_ || currentFilename_.isEmpty()) {
        return;
    }

    File file = SD.open(currentFilename_.c_str(), FILE_APPEND);
    if (!file) {
        Logger::error("DataLogger", "Failed to open file for appending");
        return;
    }

    // Prepare line
    char line[256];
    
    // Env Data
    float co2 = snapshot.environment.valid ? (float)snapshot.environment.co2Ppm : NAN;
    float temp = snapshot.environment.valid ? snapshot.environment.temperatureC : NAN;
    float rh = snapshot.environment.valid ? snapshot.environment.humidityRh : NAN;
    float press = (snapshot.environment.valid && snapshot.environment.pressureValid) ? snapshot.environment.pressureHpa : NAN;
    
    int voc = (snapshot.environment.valid && snapshot.environment.sgp41Valid) ? snapshot.environment.vocIndex : -1;
    int nox = (snapshot.environment.valid && snapshot.environment.sgp41Valid) ? snapshot.environment.noxIndex : -1;

    // PPG Data
    float hr = NAN;
    float spo2 = NAN;
    if (snapshot.ppg.state == core::PpgState::Measuring && snapshot.ppg.calculatedValid) {
        hr = snapshot.ppg.heartRateBpm;
        spo2 = snapshot.ppg.spo2Percent;
    }

    // Write line
    snprintf(line, sizeof(line), "%u,%.1f,%.2f,%.2f,%.2f,%d,%d,%.1f,%.1f",
             uptimeMs, co2, temp, rh, press, voc, nox, hr, spo2);
             
    file.println(line);
    file.close();
}

} // namespace services
