#include "drivers/sensors/bmp585_sensor.h"
#include "services/logger.h"
#include <Wire.h>
#include <Arduino.h>

namespace drivers {
namespace sensors {

// --- I2C Wrapper Functions ---
int8_t Bmp585Sensor::i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr) {
    uint8_t dev_addr = *(uint8_t*)intf_ptr;
    
    Wire.beginTransmission(dev_addr);
    Wire.write(reg_addr);
    if (Wire.endTransmission(false) != 0) { // Repeated Start
        return BMP5_E_COM_FAIL;
    }
    
    Wire.requestFrom((uint8_t)dev_addr, (size_t)length);
    
    for (uint32_t i = 0; i < length; i++) {
        reg_data[i] = Wire.read();
    }
    return BMP5_OK;
}

int8_t Bmp585Sensor::i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr) {
    uint8_t dev_addr = *(uint8_t*)intf_ptr;
    
    Wire.beginTransmission(dev_addr);
    Wire.write(reg_addr);
    for (uint32_t i = 0; i < length; i++) {
        Wire.write(reg_data[i]);
    }
    if (Wire.endTransmission() != 0) {
        return BMP5_E_COM_FAIL;
    }
    return BMP5_OK;
}

void Bmp585Sensor::delay_us(uint32_t period, void *intf_ptr) {
    (void)intf_ptr;
    if (period >= 1000) {
        delay(period / 1000);
    } else {
        delayMicroseconds(period);
    }
}

// --- Class Implementation ---

Bmp585Sensor::Bmp585Sensor() {}

bool Bmp585Sensor::begin() {
    services::Logger::info("BMP585", "Initializing BMP585 (Bosch API)...");
    state_ = core::DeviceState::Initializing;
    
    // マイコン起動直後などの安定待ち
    delay(50); // 少し長めに待機
    
    // I2Cアドレス設定 (I2Cスキャナ結果に合わせて 0x46 を設定)
    static uint8_t dev_addr = 0x46; 
    
    bmp5_dev_.intf = BMP5_I2C_INTF;
    bmp5_dev_.intf_ptr = &dev_addr;
    bmp5_dev_.read = i2c_read;
    bmp5_dev_.write = i2c_write;
    bmp5_dev_.delay_us = delay_us;
    
    // Bosch APIの bmp5_init 内部でソフトリセットが呼ばれないケースがあるため、
    // 明示的に初期化前にソフトリセットを実行し、NVMのロードを確実に行わせる
    bmp5_soft_reset(&bmp5_dev_);
    delay(20); // リセット後、NVMロードが完了するまで十分に待機
    
    int8_t rslt = BMP5_E_COM_FAIL;
    for (int i = 0; i < 5; i++) {
        rslt = bmp5_init(&bmp5_dev_);
        if (rslt == BMP5_OK) {
            break;
        }
        services::Logger::warn("BMP585", "Init failed (%d), retrying...", rslt);
        delay(100);
    }

    if (rslt != BMP5_OK) {
        services::Logger::error("BMP585", "Failed to init BMP585 after retries. Error: %d", rslt);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    // デフォルトの設定
    rslt = bmp5_get_osr_odr_press_config(&osr_odr_press_cfg_, &bmp5_dev_);
    if (rslt == BMP5_OK) {
        osr_odr_press_cfg_.osr_t = BMP5_OVERSAMPLING_8X;
        osr_odr_press_cfg_.osr_p = BMP5_OVERSAMPLING_128X;
        osr_odr_press_cfg_.odr = BMP5_ODR_10_HZ;
        osr_odr_press_cfg_.press_en = BMP5_ENABLE;
        rslt = bmp5_set_osr_odr_press_config(&osr_odr_press_cfg_, &bmp5_dev_);
    }

    if (rslt != BMP5_OK) {
        services::Logger::error("BMP585", "Failed to config BMP585. Error: %d", rslt);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    // NORMALモードへ移行
    rslt = bmp5_set_power_mode(BMP5_POWERMODE_NORMAL, &bmp5_dev_);
    if (rslt != BMP5_OK) {
        services::Logger::error("BMP585", "Failed to set power mode. Error: %d", rslt);
        state_ = core::DeviceState::Error;
        lastError_ = core::ErrorCode::InitFailed;
        return false;
    }

    services::Logger::info("BMP585", "BMP585 initialized successfully.");
    state_ = core::DeviceState::Ready;
    lastError_ = core::ErrorCode::None;
    return true;
}

void Bmp585Sensor::update(uint32_t nowMs) {
    if (state_ == core::DeviceState::Error || state_ == core::DeviceState::Offline) {
        return;
    }

    struct bmp5_sensor_data sensor_data;
    int8_t rslt = bmp5_get_sensor_data(&sensor_data, &osr_odr_press_cfg_, &bmp5_dev_);

    if (rslt != BMP5_OK) {
        services::Logger::warn("BMP585", "Failed to perform reading. Error: %d", rslt);
        lastError_ = core::ErrorCode::ReadFailed;
        hasValidData_ = false;
        return;
    }

    // 圧力データはパスカル(Pa)で返るため、100で割って hPa にする
    // struct bmp5_sensor_data { float pressure; float temperature; }; 
    // ※ 浮動小数点版が有効になっている前提（Bosch APIは浮動小数点がデフォルト）
#ifdef BMP5_USE_FIXED_POINT
    currentPressureHpa_ = (float)sensor_data.pressure / 100.0f;
#else
    currentPressureHpa_ = sensor_data.pressure / 100.0f;
#endif

    hasValidData_ = true;
    lastSuccessMs_ = nowMs;
    lastError_ = core::ErrorCode::None;
}

bool Bmp585Sensor::readEnvironment(core::EnvironmentData& out) const {
    if (!hasValidData_) return false;
    
    // 温度は SHT45 側を正とするため、ここでは気圧のみ更新
    out.pressureHpa = currentPressureHpa_;
    // タイムスタンプは新しい方とするか単純に更新
    if (lastSuccessMs_ > out.timestampMs) {
        out.timestampMs = lastSuccessMs_;
    }
    out.valid = true;
    return true;
}

} // namespace sensors
} // namespace drivers
