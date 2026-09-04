#include "storage/fram_storage.h"
#include "hal/i2c_bus.h"
#include "services/logger.h"
#include <Wire.h>
#include <algorithm>

namespace storage {

static constexpr uint8_t FRAM_I2C_ADDR_START = 0x50;
static constexpr uint8_t FRAM_I2C_ADDR_END = 0x51; // A0=L, A0=H (MAX30102 0x57 との衝突回避のため 0x51 までに制限)
static constexpr size_t I2C_CHUNK_SIZE = 32; // 安全のため32バイト単位で転送
static constexpr uint32_t FRAM_BANK_SIZE = 32768; // 1チップあたりの容量 32KB

FramStorage::FramStorage() : chipCount_(0) {
    i2cAddresses_[0] = 0;
    i2cAddresses_[1] = 0;
}

bool FramStorage::begin() {
    services::Logger::info("FramStorage", "Scanning for MB85RC256V FRAM...");
    
    hal::I2cLockGuard lock(hal::I2cDevice::Fram,
                           hal::I2cOperation::Init, 500);
    if (!lock.acquired()) {
        services::Logger::error("FramStorage", "Failed to acquire I2C lock for scanning");
        return false;
    }

    chipCount_ = 0;
    for (uint8_t addr = FRAM_I2C_ADDR_START; addr <= FRAM_I2C_ADDR_END; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            if (chipCount_ < 2) {
                i2cAddresses_[chipCount_] = addr;
                services::Logger::info("FramStorage", "FRAM detected at I2C address 0x%02X (Bank %d)", addr, chipCount_);
                chipCount_++;
            } else {
                services::Logger::warn("FramStorage", "More than 2 FRAMs detected, ignoring 0x%02X", addr);
            }
        }
    }

    if (chipCount_ == 0) {
        services::Logger::error("FramStorage", "FRAM not found in address range 0x50 - 0x57");
    } else {
        services::Logger::info("FramStorage", "Total %d FRAM chips found. Total capacity: %u bytes", chipCount_, getCapacity());
    }

    return chipCount_ > 0;
}

bool FramStorage::read(uint16_t address, uint8_t* buffer, size_t length) {
    if (chipCount_ == 0 || (uint32_t)address + length > getCapacity()) return false;

    hal::I2cLockGuard lock(hal::I2cDevice::Fram,
                           hal::I2cOperation::Read, 100);
    if (!lock.acquired()) return false;

    size_t bytesRead = 0;
    while (bytesRead < length) {
        uint32_t currentLogicalAddr = (uint32_t)address + bytesRead;
        uint8_t targetI2cAddr = 0;
        uint16_t physicalAddr = 0;

        if (currentLogicalAddr < FRAM_BANK_SIZE) {
            targetI2cAddr = i2cAddresses_[0];
            physicalAddr = (uint16_t)currentLogicalAddr;
        } else {
            targetI2cAddr = i2cAddresses_[1];
            physicalAddr = (uint16_t)(currentLogicalAddr - FRAM_BANK_SIZE);
        }

        // バンク境界（32KB）をまたぐ場合のチャンク調整
        size_t maxChunkForBank = FRAM_BANK_SIZE - physicalAddr;
        size_t remaining = length - bytesRead;
        size_t chunk = std::min({remaining, I2C_CHUNK_SIZE, maxChunkForBank});

        Wire.beginTransmission(targetI2cAddr);
        Wire.write((uint8_t)(physicalAddr >> 8));
        Wire.write((uint8_t)(physicalAddr & 0xFF));
        if (Wire.endTransmission(false) != 0) {
            hal::I2cBus::noteCommunicationError(hal::I2cDevice::Fram,
                                                hal::I2cOperation::Read);
            services::Logger::warn("FramStorage", "I2C error during read at logical 0x%05X", currentLogicalAddr);
            return false;
        }

        uint8_t bytesReceived = Wire.requestFrom((uint16_t)targetI2cAddr, (uint8_t)chunk, true);
        if (bytesReceived != chunk) {
            hal::I2cBus::noteCommunicationError(hal::I2cDevice::Fram,
                                                hal::I2cOperation::Read);
            while (Wire.available() > 0) Wire.read();
            services::Logger::warn("FramStorage", "I2C read mismatch: requested %u, got %u", chunk, bytesReceived);
            return false;
        }

        for (size_t i = 0; i < chunk; i++) {
            if (Wire.available()) {
                buffer[bytesRead + i] = Wire.read();
            } else {
                hal::I2cBus::noteCommunicationError(hal::I2cDevice::Fram,
                                                    hal::I2cOperation::Read);
                return false;
            }
        }
        bytesRead += chunk;
    }

    return true;
}

bool FramStorage::write(uint16_t address, const uint8_t* data, size_t length) {
    if (chipCount_ == 0 || (uint32_t)address + length > getCapacity()) return false;

    hal::I2cLockGuard lock(hal::I2cDevice::Fram,
                           hal::I2cOperation::Write, 100);
    if (!lock.acquired()) return false;

    size_t bytesWritten = 0;
    while (bytesWritten < length) {
        uint32_t currentLogicalAddr = (uint32_t)address + bytesWritten;
        uint8_t targetI2cAddr = 0;
        uint16_t physicalAddr = 0;

        if (currentLogicalAddr < FRAM_BANK_SIZE) {
            targetI2cAddr = i2cAddresses_[0];
            physicalAddr = (uint16_t)currentLogicalAddr;
        } else {
            targetI2cAddr = i2cAddresses_[1];
            physicalAddr = (uint16_t)(currentLogicalAddr - FRAM_BANK_SIZE);
        }

        // バンク境界（32KB）をまたぐ場合のチャンク調整
        size_t maxChunkForBank = FRAM_BANK_SIZE - physicalAddr;
        size_t remaining = length - bytesWritten;
        size_t chunk = std::min({remaining, I2C_CHUNK_SIZE, maxChunkForBank});

        Wire.beginTransmission(targetI2cAddr);
        Wire.write((uint8_t)(physicalAddr >> 8));
        Wire.write((uint8_t)(physicalAddr & 0xFF));
        
        for (size_t i = 0; i < chunk; i++) {
            Wire.write(data[bytesWritten + i]);
        }

        if (Wire.endTransmission() != 0) {
            hal::I2cBus::noteCommunicationError(hal::I2cDevice::Fram,
                                                hal::I2cOperation::Write);
            services::Logger::warn("FramStorage", "I2C error during write at logical 0x%05X", currentLogicalAddr);
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
    if (chipCount_ == 0 || (uint32_t)address + length > getCapacity()) return false;

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
    if (chipCount_ == 0 || (uint32_t)address + length > getCapacity()) return false;

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
