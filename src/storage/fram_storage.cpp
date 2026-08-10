#include "storage/fram_storage.h"
#include "hal/i2c_bus.h"
#include "services/logger.h"
#include <Wire.h>
#include <algorithm>

namespace storage {

static constexpr uint8_t FRAM_I2C_ADDR_START = 0x50;
static constexpr uint8_t FRAM_I2C_ADDR_END = 0x57;
static constexpr size_t I2C_CHUNK_SIZE = 32; // 安全のため32バイト単位で転送

FramStorage::FramStorage() : present_(false), i2cAddress_(0) {}

bool FramStorage::begin() {
    services::Logger::info("FramStorage", "Scanning for MB85RC256V FRAM...");
    
    hal::I2cLockGuard lock(500);
    if (!lock.acquired()) {
        services::Logger::error("FramStorage", "Failed to acquire I2C lock for scanning");
        return false;
    }

    present_ = false;
    for (uint8_t addr = FRAM_I2C_ADDR_START; addr <= FRAM_I2C_ADDR_END; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            i2cAddress_ = addr;
            present_ = true;
            services::Logger::info("FramStorage", "FRAM detected at I2C address 0x%02X", addr);
            break;
        }
    }

    if (!present_) {
        services::Logger::error("FramStorage", "FRAM not found in address range 0x50 - 0x57");
    }

    return present_;
}

bool FramStorage::read(uint16_t address, uint8_t* buffer, size_t length) {
    if (!present_ || address + length > MAX_CAPACITY) return false;

    hal::I2cLockGuard lock(100);
    if (!lock.acquired()) return false;

    size_t bytesRead = 0;
    while (bytesRead < length) {
        size_t chunk = std::min(length - bytesRead, I2C_CHUNK_SIZE);
        uint16_t currentAddr = address + bytesRead;

        Wire.beginTransmission(i2cAddress_);
        Wire.write((uint8_t)(currentAddr >> 8));
        Wire.write((uint8_t)(currentAddr & 0xFF));
        if (Wire.endTransmission(false) != 0) {
            services::Logger::warn("FramStorage", "I2C error during read at 0x%04X", currentAddr);
            return false;
        }

        uint8_t bytesReceived = Wire.requestFrom((uint16_t)i2cAddress_, (uint8_t)chunk, true);
        if (bytesReceived != chunk) {
            services::Logger::warn("FramStorage", "I2C read mismatch: requested %u, got %u", chunk, bytesReceived);
            return false;
        }

        for (size_t i = 0; i < chunk; i++) {
            if (Wire.available()) {
                buffer[bytesRead + i] = Wire.read();
            } else {
                return false;
            }
        }
        bytesRead += chunk;
    }

    return true;
}

bool FramStorage::write(uint16_t address, const uint8_t* data, size_t length) {
    if (!present_ || address + length > MAX_CAPACITY) return false;

    hal::I2cLockGuard lock(100);
    if (!lock.acquired()) return false;

    size_t bytesWritten = 0;
    while (bytesWritten < length) {
        size_t chunk = std::min(length - bytesWritten, I2C_CHUNK_SIZE);
        uint16_t currentAddr = address + bytesWritten;

        Wire.beginTransmission(i2cAddress_);
        Wire.write((uint8_t)(currentAddr >> 8));
        Wire.write((uint8_t)(currentAddr & 0xFF));
        
        for (size_t i = 0; i < chunk; i++) {
            Wire.write(data[bytesWritten + i]);
        }

        if (Wire.endTransmission() != 0) {
            services::Logger::warn("FramStorage", "I2C error during write at 0x%04X", currentAddr);
            return false;
        }

        bytesWritten += chunk;
    }

    return true;
}

bool FramStorage::readByte(uint16_t address, uint8_t& value) {
    return read(address, &value, 1);
}

bool FramStorage::writeByte(uint16_t address, uint8_t value) {
    return write(address, &value, 1);
}

bool FramStorage::fill(uint16_t address, uint8_t value, size_t length) {
    if (!present_ || address + length > MAX_CAPACITY) return false;

    // 最適化のため、32バイトのバッファを作成してブロック書き込み
    uint8_t buffer[I2C_CHUNK_SIZE];
    for (size_t i = 0; i < I2C_CHUNK_SIZE; i++) buffer[i] = value;

    size_t bytesWritten = 0;
    while (bytesWritten < length) {
        size_t chunk = std::min(length - bytesWritten, I2C_CHUNK_SIZE);
        if (!write(address + bytesWritten, buffer, chunk)) {
            return false;
        }
        bytesWritten += chunk;
    }
    return true;
}

bool FramStorage::verify(uint16_t address, const uint8_t* data, size_t length) {
    if (!present_ || address + length > MAX_CAPACITY) return false;

    uint8_t buffer[I2C_CHUNK_SIZE];
    size_t bytesVerified = 0;

    while (bytesVerified < length) {
        size_t chunk = std::min(length - bytesVerified, I2C_CHUNK_SIZE);
        if (!read(address + bytesVerified, buffer, chunk)) {
            return false;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (buffer[i] != data[bytesVerified + i]) {
                return false;
            }
        }
        bytesVerified += chunk;
    }

    return true;
}

} // namespace storage
