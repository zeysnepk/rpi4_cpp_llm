#include "sensor_manager.hpp"

#include "sensors/bme280.hpp"
#include "sensors/mpu6050.hpp"
#include "sensors/qmc5883l.hpp"
#include "sensors/sim_bme280.hpp"
#include "sensors/sim_mpu6050.hpp"
#include "sensors/sim_qmc5883l.hpp"

#include <iostream>

using clock_t_ = std::chrono::steady_clock;
using namespace std::chrono;

static int64_t now_ms() {
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

SensorManager::SensorManager(const nlohmann::json& config)
    : config_(config),
      mode_(config.value("mode", "real"))
{
    std::cout << "Mode: " << mode_ << "\n";

    if (mode_ == "sim") {
        // SIM MODE - donanima dokunma, fake sensor'ler yarat
        bme_ = std::make_unique<SimBME280>();
        mpu_ = std::make_unique<SimMPU6050>();
        qmc_ = std::make_unique<SimQMC5883L>();
    } else {
        // REAL MODE - GPIO + I2C + gercek sensor surucu
        auto sensors_cfg = config_.value("sensors", nlohmann::json::object());

        if (sensors_cfg.contains("power_pin")) {
            auto& pp = sensors_cfg["power_pin"];
            int  bcm         = pp.value("bcm", 13);
            bool active_high = pp.value("active_high", true);
            std::string chip = pp.value("chip", std::string("gpiochip0"));
            int  settle_ms   = pp.value("settle_ms", 200);

            power_ = std::make_unique<GPIOPower>(bcm, active_high, chip);
            if (power_->is_open() && power_->enable()) {
                std::cout << "GPIO: BCM " << bcm << " HIGH, "
                          << settle_ms << " ms bekleniyor...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
            } else {
                std::cerr << "UYARI: GPIO power enable basarisiz\n";
            }
        }

        bus_ = std::make_unique<I2CBus>("/dev/i2c-1");
        if (!bus_->is_open()) {
            std::cerr << "I2C bus acilamadi, sensorler devre disi\n";
            return;
        }

        bme_ = std::make_unique<BME280>(*bus_, 0x76);
        mpu_ = std::make_unique<MPU6050>(*bus_, 0x68);
        qmc_ = std::make_unique<QMC5883L>(*bus_, 0x0D);
    }

    // Init + registry
    auto setup = [&](Sensor* s, const std::string& key, int default_rate) {
        auto cfg = config_["sensors"].value(key, nlohmann::json::object());
        bool enabled = cfg.value("enabled", false);
        int  rate    = cfg.value("sample_rate_hz", default_rate);
        if (!enabled) {
            std::cout << "  " << key << ": disabled in config\n";
            return;
        }
        if (s->init()) {
            std::cout << "  " << key << ": online @ " << rate << " Hz\n";
            sensors_[s->name()] = SensorInfo{ s, rate, clock_t_::now(), {} };
            s->set_rate(rate);
        } else {
            std::cout << "  " << key << ": init basarisiz\n";
        }
    };

    std::cout << "Sensorler:\n";
    setup(bme_.get(), "bme280",   1);
    setup(mpu_.get(), "mpu6050", 50);
    setup(qmc_.get(), "qmc5883l", 10);
}

SensorManager::~SensorManager() { stop(); }

void SensorManager::start() {
    if (sensors_.empty()) return;
    running_ = true;
    worker_ = std::thread([this]{ run_loop(); });
}

void SensorManager::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void SensorManager::run_loop() {
    while (running_) {
        auto now = clock_t_::now();
        clock_t_::time_point next_wakeup = now + milliseconds(100);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& [name, info] : sensors_) {
                if (now >= info.next_sample) {
                    auto data = info.sensor->read();
                    if (!data.empty()) {
                        info.history.push_back({now_ms(), data});
                        if (info.history.size() > HISTORY_MAX)
                            info.history.pop_front();
                    }
                    auto period = milliseconds(1000 / std::max(1, info.rate_hz));
                    info.next_sample = now + period;
                }
                if (info.next_sample < next_wakeup) next_wakeup = info.next_sample;
            }
        }
        std::this_thread::sleep_until(next_wakeup);
    }
}

nlohmann::json SensorManager::latest_all() const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [name, info] : sensors_) {
        if (!info.history.empty()) {
            const auto& last = info.history.back();
            out[name] = {
                {"online", info.sensor->is_online()},
                {"rate_hz", info.rate_hz},
                {"timestamp_ms", last.timestamp_ms},
                {"data", last.data}
            };
        } else {
            out[name] = {{"online", info.sensor->is_online()}, {"data", nullptr}};
        }
    }
    return out;
}

nlohmann::json SensorManager::history(const std::string& name, int seconds) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sensors_.find(name);
    if (it == sensors_.end()) return nlohmann::json::array();
    int64_t cutoff = now_ms() - (int64_t)seconds * 1000;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : it->second.history) {
        if (r.timestamp_ms >= cutoff) {
            arr.push_back({{"t", r.timestamp_ms}, {"d", r.data}});
        }
    }
    return arr;
}

bool SensorManager::set_sample_rate(const std::string& name, int hz) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sensors_.find(name);
    if (it == sensors_.end()) return false;
    if (hz < 1)   hz = 1;
    if (hz > 200) hz = 200;
    it->second.rate_hz = hz;
    it->second.sensor->set_rate(hz);
    return true;
}