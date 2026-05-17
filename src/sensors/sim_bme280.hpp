#pragma once
#include "sensor.hpp"
#include <random>
#include <chrono>

class SimBME280 : public Sensor {
public:
    SimBME280();
    std::string name() const override { return "bme280"; }
    bool init() override;
    nlohmann::json read() override;

private:
    std::mt19937 rng_;
    std::chrono::steady_clock::time_point start_;
};