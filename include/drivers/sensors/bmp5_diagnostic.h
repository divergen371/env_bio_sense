#pragma once

#include "bmp5_defs.h"
#include "bmp5.h"
#include <cstdint>

namespace drivers {
namespace sensors {

class Bmp5Diagnostic {
public:
    // 診断実行 (レジスタダンプ、状態デコードを含む)
    static void runDiagnostics(struct bmp5_dev* dev);

    // 内部状態デコード用
    static void decodeStatus(uint8_t status_reg);
    static void decodeIntStatus(uint8_t int_status_reg);

private:
    static void printRegister(struct bmp5_dev* dev, uint8_t reg_addr, const char* name);
    static void printBurst(struct bmp5_dev* dev, uint8_t start_addr, uint8_t length, const char* name);
};

} // namespace sensors
} // namespace drivers
