#pragma once
#include "sensor.hpp"
#include "i2c_bus.hpp"

class MPU6050 : public Sensor {
public:
    MPU6050(I2CBus& bus, uint8_t address = 0x68);
    std::string name() const override { return "mpu6050"; }
    bool init() override;
    nlohmann::json read() override;

private:
    I2CBus& bus_;
    uint8_t addr_;
};