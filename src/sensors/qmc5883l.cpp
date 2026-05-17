#include "qmc5883l.hpp"
#include <thread>
#include <chrono>
#include <cmath>
#include <iostream>

QMC5883L::QMC5883L(I2CBus& bus, uint8_t address) : bus_(bus), addr_(address) {}

bool QMC5883L::init() {
    online_ = false;

    // Soft reset (CTRL2 register'a 0x80)
    bus_.write_byte(addr_, 0x0A, 0x80);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // SET/RESET period (datasheet onerisi)
    bus_.write_byte(addr_, 0x0B, 0x01);

    // CTRL1: OSR=512(00) | RNG=2G(00) | ODR=10Hz(00) | MODE=Continuous(01) = 0x01
    ctrl1_ = 0x01;
    if (!bus_.write_byte(addr_, 0x09, ctrl1_)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Status register erisilebiliyor mu? (0x06 chip identification = 0xFF her zaman)
    uint8_t check = 0;
    if (!bus_.read_byte(addr_, 0x0D, check)) return false;
    // 0x0D register'i chip-id'dir, beklenen deger 0xFF
    if (check != 0xFF) {
        std::cerr << "QMC5883L: chip ID 0xFF degil (0x" << std::hex << (int)check << ")\n";
        return false;
    }
    online_ = true;
    return true;
}

bool QMC5883L::set_rate(int hz) {
    if (!online_) return false;
    uint8_t odr_bits = 0x00;
    if      (hz <= 10)  odr_bits = 0x00;
    else if (hz <= 50)  odr_bits = 0x04;
    else if (hz <= 100) odr_bits = 0x08;
    else                odr_bits = 0x0C;  // 200 Hz
    ctrl1_ = (ctrl1_ & 0xF3) | odr_bits;
    return bus_.write_byte(addr_, 0x09, ctrl1_);
}

nlohmann::json QMC5883L::read() {
    if (!online_) return {};

    // Status: bit 0 = DRDY (data ready)
    uint8_t status = 0;
    if (!bus_.read_byte(addr_, 0x06, status)) return {};
    if (!(status & 0x01)) return {};  // henuz hazir degil

    uint8_t buf[6];
    if (!bus_.read_bytes(addr_, 0x00, buf, 6)) return {};

    // LITTLE endian! (datasheet)
    int16_t x = (int16_t)(buf[0] | (buf[1] << 8));
    int16_t y = (int16_t)(buf[2] | (buf[3] << 8));
    int16_t z = (int16_t)(buf[4] | (buf[5] << 8));

    // +-2G aralikta sensitivity = 12000 LSB/G
    double xg = x / 12000.0;
    double yg = y / 12000.0;
    double zg = z / 12000.0;

    // Heading (XY duzleminde, deklinasyon eklenmemis)
    double heading = std::atan2(yg, xg) * 180.0 / M_PI;
    if (heading < 0) heading += 360.0;

    return {
        {"mag_g",   {{"x", xg}, {"y", yg}, {"z", zg}}},
        {"heading_deg", heading}
    };
}