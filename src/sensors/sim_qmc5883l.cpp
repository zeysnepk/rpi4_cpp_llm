#include "sim_qmc5883l.hpp"
#include <cmath>
#include <iostream>

SimQMC5883L::SimQMC5883L()
    : rng_(std::random_device{}()),
      start_(std::chrono::steady_clock::now()) {}

bool SimQMC5883L::init() {
    std::cout << "[SIM] qmc5883l ready\n";
    online_ = true;
    return true;
}

nlohmann::json SimQMC5883L::read() {
    using namespace std::chrono;
    double t = duration_cast<duration<double>>(steady_clock::now() - start_).count();
    std::normal_distribution<double> noise(0.0, 0.01);

    // Slowly rotating compass simulation (15 degrees/second)
    double heading_rad = (t * 15.0) * M_PI / 180.0;
    double scale = 0.35;  // ~0.35 Gauss field magnitude

    double x = scale * std::cos(heading_rad) + noise(rng_);
    double y = scale * std::sin(heading_rad) + noise(rng_);
    double z = -0.32 + noise(rng_);

    double heading = std::atan2(y, x) * 180.0 / M_PI;
    if (heading < 0) heading += 360.0;

    return {
        {"mag_g",        {{"x", x}, {"y", y}, {"z", z}}},
        {"heading_deg",  heading}
    };
}