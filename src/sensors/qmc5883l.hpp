#pragma once
#include "sensor.hpp"
#include "i2c_bus.hpp"

class QMC5883L : public Sensor {
public:
    QMC5883L(I2CBus& bus, uint8_t address = 0x0D);
    std::string name() const override { return "qmc5883l"; }
    bool init() override;
    nlohmann::json read() override;

    // ODR (Output Data Rate) ayari: 10/50/100/200 Hz
    bool set_odr_hz(int hz);

private:
    I2CBus& bus_;
    uint8_t addr_;
    uint8_t ctrl1_ = 0;  // mevcut CTRL1 degerini cache'le
};