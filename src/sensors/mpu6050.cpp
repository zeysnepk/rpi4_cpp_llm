#include "mpu6050.hpp"
#include <iostream>

MPU6050::MPU6050(I2CBus& bus, uint8_t address) : bus_(bus), addr_(address) {}

bool MPU6050::init() {
    online_ = false;
    uint8_t who = 0;
    if (!bus_.read_byte(addr_, 0x75, who) || who != 0x68) {
        std::cerr << "MPU6050: chip ID yanlis (0x" << std::hex << (int)who << ")\n";
        return false;
    }
    bus_.write_byte(addr_, 0x6B, 0x00);  // PWR_MGMT_1 = 0 (uyandir)
    bus_.write_byte(addr_, 0x1B, 0x00);  // GYRO_CONFIG: +-250 deg/s
    bus_.write_byte(addr_, 0x1C, 0x00);  // ACCEL_CONFIG: +-2g
    online_ = true;
    return true;
}

nlohmann::json MPU6050::read() {
    if (!online_) return {};
    uint8_t buf[14];
    if (!bus_.read_bytes(addr_, 0x3B, buf, 14)) return {};

    auto u16 = [](uint8_t hi, uint8_t lo) -> int16_t {
        return (int16_t)((hi << 8) | lo);
    };
    int16_t ax = u16(buf[0],  buf[1]);
    int16_t ay = u16(buf[2],  buf[3]);
    int16_t az = u16(buf[4],  buf[5]);
    int16_t t  = u16(buf[6],  buf[7]);
    int16_t gx = u16(buf[8],  buf[9]);
    int16_t gy = u16(buf[10], buf[11]);
    int16_t gz = u16(buf[12], buf[13]);

    // +-2g  -> 16384 LSB/g
    // +-250 -> 131 LSB/(deg/s)
    return {
        {"accel_g",    {{"x", ax/16384.0}, {"y", ay/16384.0}, {"z", az/16384.0}}},
        {"gyro_dps",   {{"x", gx/131.0},   {"y", gy/131.0},   {"z", gz/131.0}}},
        {"temp_c",     t/340.0 + 36.53}
    };
}