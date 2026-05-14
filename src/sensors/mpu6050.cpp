#include "mpu6050.hpp"
#include <iostream>
#include <thread>
#include <chrono>

MPU6050::MPU6050(I2CBus& bus, uint8_t address) : bus_(bus), addr_(address) {}

bool MPU6050::init() {
    online_ = false;
    uint8_t who = 0;
    if (!bus_.read_byte(addr_, 0x75, who)) {
        std::cerr << "MPU: WHO_AM_I okunamadi (I2C hatasi)\n";
        return false;
    }

    // MPU6xxx / MPU9xxx ailesindeki bilinen variantlar
    const char* variant = nullptr;
    switch (who) {
        case 0x68: variant = "MPU6050"; break;
        case 0x70: variant = "MPU6500"; break;
        case 0x71: variant = "MPU9250"; break;
        case 0x73: variant = "MPU6555"; break;
        default:
            std::cerr << "MPU: bilinmeyen chip ID 0x" << std::hex << (int)who
                      << " - yine de deneniyor\n";
            variant = "unknown";
    }
    chip_id_ = who;
    std::cout << "    variant: " << variant << " (0x"
              << std::hex << (int)who << std::dec << ")\n";

    // Soft reset (her variant icin guvenli)
    bus_.write_byte(addr_, 0x6B, 0x80);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Wake up + clock source PLL
    bus_.write_byte(addr_, 0x6B, 0x00);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    bus_.write_byte(addr_, 0x1B, 0x00);  // GYRO_CONFIG: +-250 dps
    bus_.write_byte(addr_, 0x1C, 0x00);  // ACCEL_CONFIG: +-2 g

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

    // Sicaklik formulu varianta gore degisiyor
    double temp_c;
    if (chip_id_ == 0x68) {
        temp_c = t / 340.0 + 36.53;       // MPU6050
    } else {
        temp_c = t / 333.87 + 21.0;       // MPU6500/9250
    }

    return {
        {"accel_g",  {{"x", ax/16384.0}, {"y", ay/16384.0}, {"z", az/16384.0}}},
        {"gyro_dps", {{"x", gx/131.0},   {"y", gy/131.0},   {"z", gz/131.0}}},
        {"temp_c",   temp_c}
    };
}