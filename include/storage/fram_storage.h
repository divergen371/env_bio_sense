#pragma once

#include <cstdint>
#include <cstddef>

namespace storage {

class FramStorage {
public:
    FramStorage();

    // デバイスをスキャンし、初期化を行う。I2Cアドレスを自動検出する（通常 0x50 - 0x57）。
    bool begin();

    // データの読み書き (チャンク処理は内部で行う)
    bool read(uint16_t address, uint8_t* buffer, size_t length);
    bool write(uint16_t address, const uint8_t* data, size_t length);

    // バイト単位の読み書き
    bool readByte(uint16_t address, uint8_t& value);
    bool writeByte(uint16_t address, uint8_t value);

    // 特定のバイトで埋める
    bool fill(uint16_t address, uint8_t value, size_t length);

    // 書き込んだデータと一致しているか検証する
    bool verify(uint16_t address, const uint8_t* data, size_t length);

    bool isPresent() const { return chipCount_ > 0; }
    uint8_t getI2cAddress(uint8_t index = 0) const { return index < chipCount_ ? i2cAddresses_[index] : 0; }
    uint32_t getCapacity() const { return chipCount_ * 32768; }

    static constexpr size_t MAX_CAPACITY = 65536; // 64KB (32KB x 2)

private:
    uint8_t chipCount_;
    uint8_t i2cAddresses_[2];
};

} // namespace storage
