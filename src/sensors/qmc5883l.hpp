#pragma once
#include "sensor.hpp"
#include "i2c_bus.hpp"

class QMC5883L : public Sensor {
public:
    QMC5883L(I2CBus& bus, uint8_t address = 0x0D);
    std::string name() const override { return "qmc5883l"; }
    bool init() override;
    nlohmann::json read() override;
    bool set_rate(int hz) override;   // ODR donanim register'i

private:
    I2CBus& bus_;
    uint8_t addr_;
    uint8_t ctrl1_ = 0;
};