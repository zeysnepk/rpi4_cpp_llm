#pragma once
#include "sensor.hpp"
#include <random>
#include <chrono>

class SimMPU6050 : public Sensor {
public:
    SimMPU6050();
    std::string name() const override { return "mpu6050"; }
    bool init() override;
    nlohmann::json read() override;

private:
    std::mt19937 rng_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point next_spike_;
    void schedule_next_spike();
};