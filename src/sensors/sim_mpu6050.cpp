#include "sim_mpu6050.hpp"
#include <cmath>
#include <iostream>

SimMPU6050::SimMPU6050()
    : rng_(std::random_device{}()),
      start_(std::chrono::steady_clock::now())
{
    schedule_next_spike();
}

void SimMPU6050::schedule_next_spike() {
    // Simulate a motion spike every 5–15 seconds
    std::uniform_int_distribution<int> d(5000, 15000);
    next_spike_ = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(d(rng_));
}

bool SimMPU6050::init() {
    std::cout << "[SIM] mpu6050 ready (variant: SimMPU6500)\n";
    online_ = true;
    return true;
}

nlohmann::json SimMPU6050::read() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    std::normal_distribution<double> noise(0.0, 0.02);
    std::uniform_real_distribution<double> spike(-0.5, 0.5);

    // Check if it is time to inject a spike
    bool is_spike = (now >= next_spike_);
    if (is_spike) schedule_next_spike();

    double ax = noise(rng_) + (is_spike ? spike(rng_) : 0);
    double ay = noise(rng_) + (is_spike ? spike(rng_) : 0);
    double az = 1.0 + noise(rng_) + (is_spike ? spike(rng_) * 0.3 : 0);

    double gx = noise(rng_) * 30 + (is_spike ? spike(rng_) * 50 : 0);
    double gy = noise(rng_) * 30 + (is_spike ? spike(rng_) * 50 : 0);
    double gz = noise(rng_) * 30 + (is_spike ? spike(rng_) * 50 : 0);

    double temp_c = 30.0 + std::sin(
        duration_cast<duration<double>>(now - start_).count() / 60.0) * 0.5;

    return {
        {"accel_g",  {{"x", ax}, {"y", ay}, {"z", az}}},
        {"gyro_dps", {{"x", gx}, {"y", gy}, {"z", gz}}},
        {"temp_c",   temp_c}
    };
}