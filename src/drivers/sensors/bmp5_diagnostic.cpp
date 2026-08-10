#include "drivers/sensors/bmp5_diagnostic.h"
#include "services/logger.h"

namespace drivers {
namespace sensors {

void Bmp5Diagnostic::runDiagnostics(struct bmp5_dev* dev) {
    services::Logger::info("BMP5_DIAG", "===== BMP5 DIAGNOSTIC =====");
    
    // Read CHIP_ID
    uint8_t chip_id = 0;
    if (bmp5_get_regs(BMP5_REG_CHIP_ID, &chip_id, 1, dev) == BMP5_OK) {
        if (chip_id == BMP5_CHIP_ID_PRIM) {
            services::Logger::info("BMP5_DIAG", "Variant: BMP581 (0x50)");
        } else if (chip_id == BMP5_CHIP_ID_SEC) {
            services::Logger::info("BMP5_DIAG", "Variant: BMP585 (0x51)");
        } else {
            services::Logger::info("BMP5_DIAG", "Variant: UNKNOWN (0x%02X)", chip_id);
        }
    } else {
        services::Logger::error("BMP5_DIAG", "Failed to read CHIP_ID");
    }

    uint8_t rev_id = 0;
    if (bmp5_get_regs(BMP5_REG_REV_ID, &rev_id, 1, dev) == BMP5_OK) {
        services::Logger::info("BMP5_DIAG", "Revision: 0x%02X", rev_id);
    }
    
    services::Logger::info("BMP5_DIAG", "--- Registers ---");
    printRegister(dev, BMP5_REG_CHIP_ID, "CHIP_ID");
    printRegister(dev, BMP5_REG_REV_ID, "REV_ID");
    printRegister(dev, BMP5_REG_CHIP_STATUS, "CHIP_STATUS");
    printRegister(dev, BMP5_REG_DRIVE_CONFIG, "DRIVE_CONFIG");
    printRegister(dev, BMP5_REG_INT_CONFIG, "INT_CONFIG");
    printRegister(dev, BMP5_REG_INT_SOURCE, "INT_SOURCE");
    printRegister(dev, BMP5_REG_FIFO_CONFIG, "FIFO_CONFIG");
    printRegister(dev, BMP5_REG_FIFO_COUNT, "FIFO_COUNT");
    printRegister(dev, BMP5_REG_FIFO_SEL, "FIFO_SEL");
    printBurst(dev, BMP5_REG_TEMP_DATA_XLSB, 6, "DATA_BURST (TEMP+PRESS)");
    
    uint8_t int_status = 0;
    if (bmp5_get_regs(BMP5_REG_INT_STATUS, &int_status, 1, dev) == BMP5_OK) {
        decodeIntStatus(int_status);
    }
    uint8_t status = 0;
    if (bmp5_get_regs(BMP5_REG_STATUS, &status, 1, dev) == BMP5_OK) {
        decodeStatus(status);
    }
    
    printRegister(dev, BMP5_REG_DSP_CONFIG, "DSP_CONFIG");
    printRegister(dev, BMP5_REG_DSP_IIR, "DSP_IIR");
    printRegister(dev, BMP5_REG_OOR_THR_P_LSB, "OOR_THR_P_LSB");
    printRegister(dev, BMP5_REG_OOR_THR_P_MSB, "OOR_THR_P_MSB");
    printRegister(dev, BMP5_REG_OOR_RANGE, "OOR_RANGE");
    printRegister(dev, BMP5_REG_OOR_CONFIG, "OOR_CONFIG");
    printRegister(dev, BMP5_REG_OSR_CONFIG, "OSR_CONFIG");
    printRegister(dev, BMP5_REG_ODR_CONFIG, "ODR_CONFIG");
    printRegister(dev, BMP5_REG_OSR_EFF, "OSR_EFF");

    services::Logger::info("BMP5_DIAG", "===========================");
}

void Bmp5Diagnostic::decodeStatus(uint8_t status_reg) {
    // STATUS_REG (0x28)
    // Bit 0: core_rdy
    // Bit 1: nvm_rdy
    // Bit 2: nvm_err
    // Bit 3: nvm_cmd_err
    // Bit 4: boot_err_status
    services::Logger::info("BMP5_DIAG", "0x28 STATUS = 0x%02X", status_reg);
    services::Logger::info("BMP5_DIAG", "  core_rdy: %d", (status_reg & 0x01) ? 1 : 0);
    services::Logger::info("BMP5_DIAG", "  nvm_rdy: %d", (status_reg & 0x02) ? 1 : 0);
    services::Logger::info("BMP5_DIAG", "  nvm_err: %d", (status_reg & 0x04) ? 1 : 0);
    services::Logger::info("BMP5_DIAG", "  nvm_cmd_err: %d", (status_reg & 0x08) ? 1 : 0);
    services::Logger::info("BMP5_DIAG", "  boot_err_status: %d", (status_reg & 0x10) ? 1 : 0);
}

void Bmp5Diagnostic::decodeIntStatus(uint8_t int_status_reg) {
    // INT_STATUS (0x27)
    // Bit 0: drdy_data_reg
    // Bit 1: fifo_wm
    // Bit 2: fifo_full
    // Bit 3: oor_p
    // Bit 4: pors
    services::Logger::info("BMP5_DIAG", "0x27 INT_STATUS = 0x%02X", int_status_reg);
    services::Logger::info("BMP5_DIAG", "  drdy_data_reg: %d", (int_status_reg & 0x01) ? 1 : 0);
    services::Logger::info("BMP5_DIAG", "  fifo_wm: %d", (int_status_reg & 0x02) ? 1 : 0);
    services::Logger::info("BMP5_DIAG", "  fifo_full: %d", (int_status_reg & 0x04) ? 1 : 0);
    services::Logger::info("BMP5_DIAG", "  oor_p: %d", (int_status_reg & 0x08) ? 1 : 0);
    services::Logger::info("BMP5_DIAG", "  pors: %d", (int_status_reg & 0x10) ? 1 : 0);
}

void Bmp5Diagnostic::printRegister(struct bmp5_dev* dev, uint8_t reg_addr, const char* name) {
    uint8_t val = 0;
    if (bmp5_get_regs(reg_addr, &val, 1, dev) == BMP5_OK) {
        services::Logger::info("BMP5_DIAG", "0x%02X %-20s = 0x%02X", reg_addr, name, val);
    } else {
        services::Logger::info("BMP5_DIAG", "0x%02X %-20s = READ FAIL", reg_addr, name);
    }
}

void Bmp5Diagnostic::printBurst(struct bmp5_dev* dev, uint8_t start_addr, uint8_t length, const char* name) {
    uint8_t data[16] = {0}; // safe max for our use cases
    if (length > 16) length = 16;
    if (bmp5_get_regs(start_addr, data, length, dev) == BMP5_OK) {
        char buf[64] = {0};
        int pos = 0;
        for (uint8_t i = 0; i < length; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", data[i]);
        }
        services::Logger::info("BMP5_DIAG", "0x%02X %-20s = %s", start_addr, name, buf);
    } else {
        services::Logger::info("BMP5_DIAG", "0x%02X %-20s = READ FAIL", start_addr, name);
    }
}

} // namespace sensors
} // namespace drivers
