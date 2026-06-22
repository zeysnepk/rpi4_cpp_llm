#pragma once
#include <nlohmann/json.hpp>
#include <string>

/**
 * Abstract base class for all sensors (real and simulated).
 * Each sensor implements init(), read(), and optionally set_rate().
 */
class Sensor {
public:
    virtual ~Sensor() = default;
    virtual std::string name() const = 0;
    virtual bool init() = 0;
    virtual nlohmann::json read() = 0;

    // Hardware ODR control (e.g. QMC5883L). Default: no-op, rate is controlled by polling interval.
    virtual bool set_rate(int /*hz*/) { return true; }

    bool is_online() const { return online_; }

protected:
    bool online_ = false;
};