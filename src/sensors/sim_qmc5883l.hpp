#pragma once
#include "sensor.hpp"
#include <random>
#include <chrono>

class SimQMC5883L : public Sensor {
public:
    SimQMC5883L();
    std::string name() const override { return "qmc5883l"; }
    bool init() override;
    nlohmann::json read() override;

private:
    std::mt19937 rng_;
    std::chrono::steady_clock::time_point start_;
};