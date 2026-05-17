#pragma once
#include <nlohmann/json.hpp>
#include <string>

class Sensor {
public:
    virtual ~Sensor() = default;
    virtual std::string name() const = 0;
    virtual bool init() = 0;
    virtual nlohmann::json read() = 0;

    // Hardware-specific rate degisiklikleri icin (QMC ODR gibi)
    // Default: no-op (yazilim tarafinda zaten polling rate degisiyor)
    virtual bool set_rate(int /*hz*/) { return true; }

    bool is_online() const { return online_; }

protected:
    bool online_ = false;
};