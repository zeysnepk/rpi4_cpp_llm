#include "tool_dispatcher.hpp"
#include <fstream>
#include <sstream>
#include <numeric>
#include <algorithm>
#include <iostream>

using json = nlohmann::json;

ToolDispatcher::ToolDispatcher(SensorManager& sensors, std::string config_path)
    : sensors_(sensors), config_path_(std::move(config_path)) {}

json ToolDispatcher::get_tool_definitions() const {
    return json::array({
        {
            {"type", "function"},
            {"function", {
                {"name", "get_current"},
                {"description",
                    "Belirtilen sensorun anlik son okumasini doner. "
                    "BME280: sicaklik (°C), nem (%), basinc (hPa). "
                    "MPU6500: ivme (g, xyz), gyro (°/s, xyz), sicaklik (°C). "
                    "QMC5883L: manyetik alan (Gauss, xyz), heading (derece). "
                    "Veriler 'data' alaninda gelir."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"sensor", {
                            {"type", "string"},
                            {"enum", {"bme280", "mpu6050", "qmc5883l", "all"}},
                            {"description", "bme280, mpu6050, qmc5883l veya 'all' (hepsi)"}
                        }}
                    }},
                    {"required", json::array({"sensor"})}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "get_history_stats"},
                {"description",
                    "Bir sensorun belirli bir metrigi icin son N saniyedeki "
                    "min/max/ortalama/son deger istatistiklerini doner. "
                    "Iç ic gecmis alanlar nokta ile: 'accel_g.z', 'mag_g.x' gibi."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"sensor",  {{"type", "string"},
                                     {"enum", {"bme280", "mpu6050", "qmc5883l"}}}},
                        {"metric",  {{"type", "string"},
                                     {"description", "Ornek: temperature_c, humidity_pct, "
                                                     "accel_g.x, gyro_dps.z, mag_g.y, heading_deg"}}},
                        {"seconds", {{"type", "integer"},
                                     {"minimum", 1}, {"maximum", 600}}}
                    }},
                    {"required", json::array({"sensor", "metric", "seconds"})}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "set_sample_rate"},
                {"description",
                    "Bir sensorun ornekleme hizini Hz olarak degistirir ve config.json'a kaydeder. "
                    "BME280 icin 1-25 Hz, MPU6500 icin 1-200 Hz, "
                    "QMC5883L icin sadece 10/50/100/200 Hz (en yakini secilir)."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"sensor", {{"type", "string"},
                                    {"enum", {"bme280", "mpu6050", "qmc5883l"}}}},
                        {"hz",     {{"type", "integer"},
                                    {"minimum", 1}, {"maximum", 200}}}
                    }},
                    {"required", json::array({"sensor", "hz"})}
                }}
            }}
        }
    });
}

json ToolDispatcher::execute(const std::string& name, const json& args) {
    try {
        if (name == "get_current")        return tool_get_current(args);
        if (name == "get_history_stats")  return tool_get_history_stats(args);
        if (name == "set_sample_rate")    return tool_set_sample_rate(args);
        return {{"error", "Bilinmeyen tool: " + name}};
    } catch (const std::exception& e) {
        return {{"error", std::string("Tool hatasi: ") + e.what()}};
    }
}

json ToolDispatcher::tool_get_current(const json& args) {
    std::string sensor = args.at("sensor");
    auto latest = sensors_.latest_all();
    if (sensor == "all") return latest;
    if (latest.contains(sensor)) return latest[sensor];
    return {{"error", "Sensor bulunamadi: " + sensor}};
}

// Iç ic JSON alanina nokta ile eris: "accel_g.x"
static const json* dig(const json& root, const std::string& path) {
    const json* cur = &root;
    std::stringstream ss(path);
    std::string key;
    while (std::getline(ss, key, '.')) {
        if (!cur->is_object() || !cur->contains(key)) return nullptr;
        cur = &(*cur)[key];
    }
    return cur;
}

json ToolDispatcher::tool_get_history_stats(const json& args) {
    std::string sensor = args.at("sensor");
    std::string metric = args.at("metric");
    int seconds = args.at("seconds");

    auto hist = sensors_.history(sensor, seconds);
    std::vector<double> vals;
    vals.reserve(hist.size());
    for (const auto& entry : hist) {
        if (!entry.contains("d")) continue;
        const json* v = dig(entry["d"], metric);
        if (v && v->is_number()) vals.push_back(v->get<double>());
    }

    if (vals.empty()) {
        return {{"error", "Veri yok: " + sensor + "." + metric +
                          " (son " + std::to_string(seconds) + " sn)"}};
    }

    double sum  = std::accumulate(vals.begin(), vals.end(), 0.0);
    double avg  = sum / vals.size();
    double vmin = *std::min_element(vals.begin(), vals.end());
    double vmax = *std::max_element(vals.begin(), vals.end());

    return {
        {"sensor",  sensor},
        {"metric",  metric},
        {"seconds", seconds},
        {"samples", (int)vals.size()},
        {"min",     vmin},
        {"max",     vmax},
        {"avg",     avg},
        {"last",    vals.back()}
    };
}

json ToolDispatcher::tool_set_sample_rate(const json& args) {
    std::string sensor = args.at("sensor");
    int hz = args.at("hz");
    if (hz < 1) hz = 1;
    if (hz > 200) hz = 200;

    if (!sensors_.set_sample_rate(sensor, hz)) {
        return {{"error", "Sensor yok veya hata: " + sensor}};
    }

    // Config'i guncelle ve diske yaz
    auto cfg = load_config();
    if (cfg.contains("sensors") && cfg["sensors"].contains(sensor)) {
        cfg["sensors"][sensor]["sample_rate_hz"] = hz;
        if (!save_config(cfg)) {
            return {
                {"ok", true},
                {"sensor", sensor},
                {"hz_applied", hz},
                {"warning", "Bellekte degisti ama config.json'a yazilamadi"}
            };
        }
    }

    return {
        {"ok", true},
        {"sensor", sensor},
        {"hz_applied", hz},
        {"persisted", true}
    };
}

json ToolDispatcher::load_config() {
    std::ifstream f(config_path_);
    if (!f) return json::object();
    try { return json::parse(f); } catch (...) { return json::object(); }
}

bool ToolDispatcher::save_config(const json& cfg) {
    std::ofstream f(config_path_);
    if (!f) return false;
    f << cfg.dump(2);
    return f.good();
}