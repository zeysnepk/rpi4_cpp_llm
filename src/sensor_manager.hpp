#pragma once
#include "sensors/sensor.hpp"
#include "sensors/i2c_bus.hpp"
#include "sensors/bme280.hpp"
#include "sensors/mpu6050.hpp"
#include "sensors/qmc5883l.hpp"

#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <chrono>

#include "gpio_power.hpp"
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

    // Anlik (en son) tum sensor degerleri
    nlohmann::json latest_all() const;

    // Belirli bir sensorun son N saniyelik gecmisi (grafik icin)
    nlohmann::json history(const std::string& sensor_name, int seconds) const;

    // Config'i guncelle (LLM bunu cagiracak ileride)
    bool set_sample_rate(const std::string& sensor_name, int hz);

private:
    std::unique_ptr<GPIOPower> power_;
    void run_loop();

    nlohmann::json config_;
    I2CBus bus_;
    BME280   bme_;
    MPU6050  mpu_;
    QMC5883L qmc_;

    struct SensorInfo {
        Sensor* sensor;
        int rate_hz;
        std::chrono::steady_clock::time_point next_sample;
        std::deque<SensorReading> history;
    };
    std::unordered_map<std::string, SensorInfo> sensors_;

    mutable std::mutex mtx_;
    std::atomic<bool> running_{false};
    std::thread worker_;

    static constexpr size_t HISTORY_MAX = 6000;  // ~600 sn @ 10 Hz
};