#pragma once
#include <nlohmann/json.hpp>
#include <string>

class Sensor {
public:
    virtual ~Sensor() = default;
    virtual std::string name() const = 0;
    virtual bool init() = 0;
    virtual nlohmann::json read() = 0;  // okuma yapamazsa {} doner
    bool is_online() const { return online_; }

protected:
    bool online_ = false;
};