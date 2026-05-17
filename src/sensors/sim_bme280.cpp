#include "sim_bme280.hpp"
#include <cmath>
#include <iostream>

SimBME280::SimBME280()
    : rng_(std::random_device{}()),
      start_(std::chrono::steady_clock::now()) {}

bool SimBME280::init() {
    std::cout << "    [SIM] bme280 hazir\n";
    online_ = true;
    return true;
}

nlohmann::json SimBME280::read() {
    using namespace std::chrono;
    double t = duration_cast<duration<double>>(steady_clock::now() - start_).count();
    std::uniform_real_distribution<double> n(-0.05, 0.05);

    // Yavas sinusoid + gurultu - gercekci ortam degerleri
    double temp  = 25.0 + 2.0  * std::sin(t / 60.0)        + n(rng_);
    double humid = 45.0 + 10.0 * std::sin(t / 120.0 + 1.0) + n(rng_) * 3;
    double pres  = 1013.0 + 2.0 * std::sin(t / 300.0)      + n(rng_) * 0.5;

    return {
        {"temperature_c", temp},
        {"humidity_pct",  humid},
        {"pressure_hpa",  pres}
    };
}