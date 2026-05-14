#pragma once
#include "sensor.hpp"
#include "i2c_bus.hpp"

class BME280 : public Sensor {
public:
    BME280(I2CBus& bus, uint8_t address = 0x76);
    std::string name() const override { return "bme280"; }
    bool init() override;
    nlohmann::json read() override;

private:
    I2CBus& bus_;
    uint8_t addr_;

    // Kalibrasyon katsayilari (datasheet'ten)
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1, H3; int16_t H2, H4, H5; int8_t H6;

    bool read_calibration();
};