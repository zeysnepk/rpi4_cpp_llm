#pragma once
#include "sensors/sensor.hpp"
#include "sensors/i2c_bus.hpp"
#include "gpio_power.hpp"

#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <chrono>
#include <memory>

struct SensorReading {
    int64_t timestamp_ms;
    nlohmann::json data;
};

class SensorManager {
public:
    SensorManager(const nlohmann::json& config);
    ~SensorManager();

    void start();
    void stop();

    nlohmann::json latest_all() const;
    nlohmann::json history(const std::string& sensor_name, int seconds) const;
    nlohmann::json latest_n(const std::string& sensor_name, int count) const;
    bool set_sample_rate(const std::string& sensor_name, int hz);
    bool set_enabled(const std::string& sensor_name, bool enabled);

    std::string mode() const { return mode_; }

private:
    void run_loop();

    nlohmann::json config_;
    std::string mode_;   // "real" | "sim"

    // Real-mode only (sim'de nullptr)
    std::unique_ptr<I2CBus>    bus_;
    std::unique_ptr<GPIOPower> power_;

    // Sensorler - polymorphic (real veya sim)
    std::unique_ptr<Sensor> bme_;
    std::unique_ptr<Sensor> mpu_;
    std::unique_ptr<Sensor> qmc_;

    struct SensorInfo {
        Sensor* sensor;
        int rate_hz;
        std::chrono::steady_clock::time_point next_sample;
        std::deque<SensorReading> history;
        bool enabled = true;
    };
    std::unordered_map<std::string, SensorInfo> sensors_;

    mutable std::mutex mtx_;
    std::atomic<bool> running_{false};
    std::thread worker_;

    static constexpr size_t HISTORY_MAX = 6000;
};