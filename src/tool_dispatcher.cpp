#include "tool_dispatcher.hpp"
#include <fstream>
#include <sstream>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>

using json = nlohmann::json;

ToolDispatcher::ToolDispatcher(SensorManager& sensors,
                               const Analyzer& analyzer,
                               std::string config_path)
    : sensors_(sensors), analyzer_(analyzer),
      config_path_(std::move(config_path)) {}

// ============================================================
// TOOL TANIMLARI (eski API uyumlu, IntentRouter cagiriyor)
// ============================================================
json ToolDispatcher::get_tool_definitions() const {
    return json::array({
        {
            {"type", "function"},
            {"function", {
                {"name", "get_current"},
                {"description", "Sensorun anlik son okumasi"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"sensor", {{"type","string"},
                                    {"enum",{"bme280","mpu6050","qmc5883l","all"}}}}
                    }},
                    {"required", json::array({"sensor"})}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "get_history_stats"},
                {"description", "Bir sensor metriginin son N sn istatistigi (min/max/avg/trend)"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"sensor",  {{"type","string"}}},
                        {"metric",  {{"type","string"}}},
                        {"seconds", {{"type","integer"},{"minimum",1},{"maximum",600}}}
                    }},
                    {"required", json::array({"sensor","metric","seconds"})}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "set_sample_rate"},
                {"description", "Sensor ornekleme hizini degistir"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"sensor", {{"type","string"}}},
                        {"hz",     {{"type","integer"},{"minimum",1},{"maximum",200}}}
                    }},
                    {"required", json::array({"sensor","hz"})}
                }}
            }}
        }
    });
}

// ============================================================
// EXECUTE
// ============================================================
json ToolDispatcher::execute(const std::string& name, const json& args) {
    try {
        if (name == "get_current")        return tool_get_current(args);
        if (name == "get_history_stats")  return tool_get_history_stats(args);
        if (name == "set_sample_rate")    return tool_set_sample_rate(args);
        if (name == "set_threshold")      return tool_set_threshold(args);
        if (name == "set_sensor_enabled") return tool_set_sensor_enabled(args);
        if (name == "get_config")         return tool_get_config(args);
        return {{"error","Unknown tool: " + name}};
    } catch (const std::exception& e) {
        return {{"error", std::string("Tool error: ") + e.what()}};
    }
}

// ============================================================
// GET CURRENT (+ analyzer enrichment)
// ============================================================
json ToolDispatcher::tool_get_current(const json& args) {
    std::string sensor = args.at("sensor");
    auto latest = sensors_.latest_all();
    auto cfg = load_config();

    auto is_enabled = [&](const std::string& s) -> bool {
        if (!cfg.contains("sensors") || !cfg["sensors"].contains(s)) return true;
        return cfg["sensors"][s].value("enabled", true);
    };

    if (sensor == "all") {
        // Disabled sensor'lari filtrele, kalanlar icin analiz ekle
        for (auto it = latest.begin(); it != latest.end(); ) {
            if (!is_enabled(it.key())) { it = latest.erase(it); continue; }
            auto& info = it.value();
            if (info.contains("data") && !info["data"].is_null()) {
                json analyses = json::object();
                for (auto& [metric, value] : info["data"].items()) {
                    if (!value.is_number()) continue;
                    auto a = analyzer_.analyze(it.key(), metric, value.get<double>());
                    if (!a.empty()) analyses[metric] = a;
                }
                if (!analyses.empty()) info["analysis"] = analyses;
            }
            ++it;
        }
        return latest;
    }

    if (!is_enabled(sensor))
        return {{"error", sensor + " sensor is disabled. Enable it first."}};

    if (!latest.contains(sensor))
        return {{"error","Sensor not found: " + sensor}};

    auto out = latest[sensor];
    // Tek sensor: her metrigi analize tabi tut
    if (out.contains("data") && !out["data"].is_null()) {
        json analyses = json::object();
        for (auto& [metric, value] : out["data"].items()) {
            if (!value.is_number()) continue;
            auto a = analyzer_.analyze(sensor, metric, value.get<double>());
            if (!a.empty()) analyses[metric] = a;
        }
        if (!analyses.empty()) out["analysis"] = analyses;
    }
    return out;
}

// ============================================================
// HISTORY STATS (+ trend slope + analyzer)
// ============================================================
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

// Linear regression slope (deger/saniye)
static double compute_slope(const std::vector<double>& ts_sec,
                             const std::vector<double>& vals) {
    if (ts_sec.size() < 2) return 0.0;
    size_t n = ts_sec.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
        sx  += ts_sec[i];
        sy  += vals[i];
        sxx += ts_sec[i] * ts_sec[i];
        sxy += ts_sec[i] * vals[i];
    }
    double denom = n * sxx - sx * sx;
    if (std::fabs(denom) < 1e-12) return 0.0;
    return (n * sxy - sx * sy) / denom;
}

json ToolDispatcher::tool_get_history_stats(const json& args) {
    std::string sensor = args.at("sensor");
    std::string metric = args.at("metric");
    int seconds = args.at("seconds");

    // Disabled sensor kontrolu
    {
        auto cfg = load_config();
        if (cfg.contains("sensors") && cfg["sensors"].contains(sensor)) {
            if (!cfg["sensors"][sensor].value("enabled", true))
                return {{"error", sensor + " sensor is disabled. Enable it first."}};
        }
    }

    auto hist = sensors_.history(sensor, seconds);

    std::vector<double> ts_sec, vals;
    ts_sec.reserve(hist.size());
    vals.reserve(hist.size());

    double t0_ms = -1;
    for (const auto& entry : hist) {
        if (!entry.contains("d") || !entry.contains("t")) continue;
        const json* v = dig(entry["d"], metric);
        if (!v || !v->is_number()) continue;
        double t_ms = entry["t"].get<double>();
        if (t0_ms < 0) t0_ms = t_ms;
        ts_sec.push_back((t_ms - t0_ms) / 1000.0);
        vals.push_back(v->get<double>());
    }

    if (vals.empty()) {
        return {{"error","No data: " + sensor + "." + metric +
                       " (last " + std::to_string(seconds) + " sec)"}};
    }

    double sum  = std::accumulate(vals.begin(), vals.end(), 0.0);
    double avg  = sum / vals.size();
    double vmin = *std::min_element(vals.begin(), vals.end());
    double vmax = *std::max_element(vals.begin(), vals.end());
    double last = vals.back();
    double slope = compute_slope(ts_sec, vals);  // deger/sn
    double change = vals.back() - vals.front();
    double change_pct = (std::fabs(vals.front()) > 1e-9)
        ? (change / vals.front()) * 100.0 : 0.0;

    // Trend direction
    double range = vmax - vmin;
    double slope_abs = std::fabs(slope);
    std::string direction;
    if (range < 1e-6 || slope_abs * seconds < range * 0.05) direction = "stable";
    else if (slope > 0) direction = "rising";
    else                direction = "falling";

    json result = {
        {"sensor",     sensor},
        {"metric",     metric},
        {"seconds",    seconds},
        {"samples",    (int)vals.size()},
        {"min",        vmin},
        {"max",        vmax},
        {"avg",        avg},
        {"last",       last},
        {"trend", {
            {"direction",     direction},
            {"slope_per_sec", slope},
            {"change",        change},
            {"change_pct",    change_pct}
        }}
    };

    // Analyzer eklentisi
    auto an = analyzer_.analyze_stats(sensor, metric, vmin, vmax, avg, last);
    if (!an.empty()) result["analysis"] = an;

    return result;
}

// ============================================================
// SET SAMPLE RATE (degisiklik yok)
// ============================================================
json ToolDispatcher::tool_set_sample_rate(const json& args) {
    std::string sensor = args.at("sensor");
    int hz = args.at("hz");
    if (hz < 1) hz = 1;
    if (hz > 200) hz = 200;

    if (!sensors_.set_sample_rate(sensor, hz)) {
        return {{"error","Sensor not found or error: " + sensor}};
    }

    auto cfg = load_config();
    if (cfg.contains("sensors") && cfg["sensors"].contains(sensor)) {
        cfg["sensors"][sensor]["sample_rate_hz"] = hz;
        if (!save_config(cfg)) {
            return {{"ok",true},{"sensor",sensor},{"hz_applied",hz},
                    {"warning","Changed in memory but could not save to config"}};
        }
    }

    return {{"ok",true},{"sensor",sensor},{"hz_applied",hz},{"persisted",true}};
}

// ============================================================
// SET THRESHOLD — bir metrigin anomali esik degerlerini degistirir
// args: {metric: "bme280.temperature_c", min: 10, max: 40}
// min/max ikisi birden veya yalnizca biri gelebilir.
// ============================================================
json ToolDispatcher::tool_set_threshold(const json& args) {
    std::string metric = args.value("metric", "");
    if (metric.empty()) return {{"error","metric parameter missing"}};

    auto cfg = load_config();
    if (!cfg.contains("thresholds") || !cfg["thresholds"].contains(metric)) {
        return {{"error","Unknown metric: " + metric +
                 ". Valid metrics: bme280.temperature_c, bme280.humidity_pct, "
                 "bme280.pressure_hpa, mpu6050.accel_g.x/y/z, mpu6050.gyro_dps.x/y/z, "
                 "mpu6050.temp_c, qmc5883l.heading_deg"}};
    }

    json& thr = cfg["thresholds"][metric];
    json changed = json::object();

    if (args.contains("min") && args["min"].is_number()) {
        double old_min = thr.value("min", 0.0);
        thr["min"] = args["min"].get<double>();
        changed["min"] = {{"old", old_min}, {"new", thr["min"]}};
    }
    if (args.contains("max") && args["max"].is_number()) {
        double old_max = thr.value("max", 0.0);
        thr["max"] = args["max"].get<double>();
        changed["max"] = {{"old", old_max}, {"new", thr["max"]}};
    }

    if (changed.empty()) return {{"error","No min or max value provided"}};

    if (!save_config(cfg)) {
        return {{"ok",true},{"metric",metric},{"changed",changed},
                {"warning","Changed in memory but could not save to config"},{"persisted",false}};
    }

    // Analyzer'ı da güncelle (hot-reload: config yeniden okunacak)
    return {{"ok",true},{"metric",metric},{"changed",changed},
            {"label", thr.value("label","")},{"persisted",true}};
}

// ============================================================
// SET SENSOR ENABLED — sensor'u ac/kapat
// args: {sensor: "bme280", enabled: true/false}
// ============================================================
json ToolDispatcher::tool_set_sensor_enabled(const json& args) {
    std::string sensor = args.value("sensor", "");
    if (sensor.empty()) return {{"error","sensor parameter missing"}};
    bool enabled = args.value("enabled", true);

    auto cfg = load_config();
    if (!cfg.contains("sensors") || !cfg["sensors"].contains(sensor)) {
        return {{"error","Unknown sensor: " + sensor}};
    }

    bool was_enabled = cfg["sensors"][sensor].value("enabled", true);
    cfg["sensors"][sensor]["enabled"] = enabled;

    if (!save_config(cfg)) {
        return {{"ok",true},{"sensor",sensor},{"enabled",enabled},
                {"warning","Changed in memory but could not save to config"},{"persisted",false}};
    }

    return {{"ok",true},{"sensor",sensor},{"enabled",enabled},
            {"was_enabled",was_enabled},{"persisted",true}};
}

// ============================================================
// GET CONFIG — tum ayarlanabilir parametreleri dondurur
// ============================================================
json ToolDispatcher::tool_get_config(const json& /*args*/) {
    auto cfg = load_config();
    json result = json::object();

    // Sensor ornekleme hizlari ve durumlari
    if (cfg.contains("sensors")) {
        result["sensors"] = json::object();
        for (auto& [s, info] : cfg["sensors"].items()) {
            result["sensors"][s] = {
                {"sample_rate_hz", info.value("sample_rate_hz", 0)},
                {"enabled",        info.value("enabled", true)}
            };
        }
    }

    // Anomali esik degerleri
    if (cfg.contains("thresholds")) {
        result["thresholds"] = cfg["thresholds"];
    }

    result["mode"] = cfg.value("mode", "unknown");
    return result;
}

// ============================================================
// FORMAT_FOR_LLM
// Sends rounded English text to the LLM, not raw JSON.
// ============================================================
static std::string round_str(double v, int digits = 2) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(digits) << v;
    return os.str();
}

// Human-readable metric label (English)
static std::string metric_label(const std::string& sensor,
                                 const std::string& metric) {
    if (sensor == "bme280") {
        if (metric == "temperature_c") return "temperature";
        if (metric == "humidity_pct")  return "humidity";
        if (metric == "pressure_hpa")  return "pressure";
    }
    if (sensor == "mpu6050") {
        if (metric == "accel_g.x") return "X acceleration";
        if (metric == "accel_g.y") return "Y acceleration";
        if (metric == "accel_g.z") return "Z acceleration";
        if (metric == "gyro_dps.x") return "X gyro";
        if (metric == "gyro_dps.y") return "Y gyro";
        if (metric == "gyro_dps.z") return "Z gyro";
        if (metric == "temp_c")    return "MPU temperature";
    }
    if (sensor == "qmc5883l") {
        if (metric == "heading_deg") return "heading";
        if (metric == "mag_g.x")     return "X magnetic";
        if (metric == "mag_g.y")     return "Y magnetic";
        if (metric == "mag_g.z")     return "Z magnetic";
    }
    return metric;
}

static std::string unit_for(const std::string& metric) {
    if (metric == "temperature_c" || metric == "temp_c") return "°C";
    if (metric == "humidity_pct")  return "%";
    if (metric == "pressure_hpa")  return "hPa";
    if (metric.rfind("accel_g", 0) == 0) return "g";
    if (metric.rfind("gyro_dps", 0) == 0) return "°/s";
    if (metric.rfind("mag_g", 0) == 0) return "G";
    if (metric == "heading_deg") return "°";
    return "";
}

static std::string heading_to_compass(double deg) {
    deg = std::fmod(deg + 360.0, 360.0);
    static const char* dirs[] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW"
    };
    int idx = (int)std::round(deg / 45.0) % 8;
    return dirs[idx];
}

std::string ToolDispatcher::format_for_llm(const std::string& tool_name,
                                            const json& tr) const {
    if (tr.contains("error")) {
        return "ERROR: " + tr["error"].get<std::string>();
    }

    std::ostringstream os;

    if (tool_name == "set_threshold") {
        if (tr.value("ok", false)) {
            std::string metric = tr.value("metric","?");
            std::string label  = tr.value("label", metric);
            os << "Threshold updated: " << label << " (" << metric << ")";
            if (tr.contains("changed")) {
                const auto& ch = tr["changed"];
                if (ch.contains("min"))
                    os << " | min: " << ch["min"]["old"].dump()
                       << " -> " << ch["min"]["new"].dump();
                if (ch.contains("max"))
                    os << " | max: " << ch["max"]["old"].dump()
                       << " -> " << ch["max"]["new"].dump();
            }
            if (tr.value("persisted",false)) os << " | Saved to config.";
        } else {
            os << "Tool failed.";
        }
        return os.str();
    }

    if (tool_name == "set_sensor_enabled") {
        if (tr.value("ok", false)) {
            std::string s = tr.value("sensor","?");
            bool en = tr.value("enabled", true);
            os << s << " sensor " << (en ? "ENABLED" : "DISABLED") << ".";
            if (tr.value("persisted",false)) os << " Saved to config.";
        } else {
            os << "Tool failed.";
        }
        return os.str();
    }

    if (tool_name == "get_config") {
        os << "Current system settings:\n\n";
        os << "[Sensor Sample Rates]\n";
        if (tr.contains("sensors")) {
            for (auto& [s, info] : tr["sensors"].items()) {
                bool en = info.value("enabled", true);
                os << "- " << s << ": " << info.value("sample_rate_hz",0) << " Hz"
                   << (en ? "" : " (DISABLED)") << "\n";
            }
        }
        os << "\n[Anomaly Thresholds]\n";
        if (tr.contains("thresholds")) {
            for (auto& [key, thr] : tr["thresholds"].items()) {
                std::string lbl = thr.value("label", key);
                std::string u = unit_for(key.substr(key.find('.')+1));
                double mn = thr.value("min", 0.0), mx = thr.value("max", 0.0);
                os << "- " << lbl << " (" << key << "): "
                   << "min=" << round_str(mn,1) << u
                   << ", max=" << round_str(mx,1) << u << "\n";
            }
        }
        os << "\n[Mode] " << tr.value("mode","?") << "\n";
        os << "\nTo change any value, start your message with 'set'. Examples:\n";
        os << "  set temperature max to 35\n";
        os << "  set bme sample rate 10 hz\n";
        os << "  set mpu disabled\n";
        return os.str();
    }

    if (tool_name == "set_sample_rate") {
        if (tr.value("ok", false)) {
            os << "Result: " << tr.value("sensor","?")
               << " set to " << tr.value("hz_applied",0) << " Hz.";
            if (tr.value("persisted", false))
                os << " Saved to config.";
        } else {
            os << "Tool failed.";
        }
        return os.str();
    }

    if (tool_name == "get_current") {
        // Helper: nested + flat metric'leri tek seferde yaz
        auto write_metrics = [&os](const std::string& sensor,
                                    const json& data,
                                    const json* analysis,
                                    const std::string& indent) {
            for (auto& [m, v] : data.items()) {
                if (v.is_number()) {
                    os << indent << "- " << metric_label(sensor, m) << ": "
                       << round_str(v.get<double>(), 2) << " " << unit_for(m);
                    if (m == "heading_deg") {
                        os << " (" << heading_to_compass(v.get<double>()) << ")";
                    }
                    if (analysis && analysis->contains(m)) {
                        os << " [" << (*analysis)[m].value("status","?") << "]";
                    }
                    os << "\n";
                } else if (v.is_object()) {
                    for (auto& [sub, vv] : v.items()) {
                        if (!vv.is_number()) continue;
                        std::string full = m + "." + sub;
                        os << indent << "- " << metric_label(sensor, full) << ": "
                           << round_str(vv.get<double>(), 2) << " " << unit_for(full);
                        if (full == "heading_deg") {
                            os << " (" << heading_to_compass(vv.get<double>()) << ")";
                        }
                        os << "\n";
                    }
                }
            }
        };

        if (tr.contains("data") && tr["data"].is_object()) {
            // Single sensor (data + sibling analysis)
            os << "Sensor data:\n";
            const json* a = tr.contains("analysis") ? &tr["analysis"] : nullptr;
            write_metrics("", tr["data"], a, "");
        } else {
            // "all" sensors
            os << "All sensor data:\n";
            for (auto& [sname, info] : tr.items()) {
                if (!info.contains("data") || info["data"].is_null()) continue;
                os << sname << ":\n";
                const json* a = info.contains("analysis") ? &info["analysis"] : nullptr;
                write_metrics(sname, info["data"], a, "  ");
            }
        }
        return os.str();
    }

    if (tool_name == "get_history_stats") {
        std::string s = tr.value("sensor", "");
        std::string m = tr.value("metric", "");
        std::string u = unit_for(m);
        int sec = tr.value("seconds", 0);
        int n = tr.value("samples", 0);

        os << "Last " << sec << " seconds - " << metric_label(s, m) << ":\n";
        os << "- Average: " << round_str(tr.value("avg",0.0), 2) << " " << u << "\n";
        os << "- Min: " << round_str(tr.value("min",0.0), 2) << " " << u << "\n";
        os << "- Max: " << round_str(tr.value("max",0.0), 2) << " " << u << "\n";
        os << "- Last reading: " << round_str(tr.value("last",0.0), 2) << " " << u << "\n";
        os << "- Samples: " << n << "\n";

        if (tr.contains("trend")) {
            auto& tnd = tr["trend"];
            os << "- Trend: " << tnd.value("direction","?")
               << " (total change "
               << round_str(tnd.value("change",0.0), 2) << " " << u;
            double pct = tnd.value("change_pct", 0.0);
            if (std::fabs(pct) > 0.1)
                os << ", " << round_str(pct, 1) << "%";
            os << ")\n";
        }

        if (tr.contains("analysis")) {
            auto& a = tr["analysis"];
            bool anom = a.value("anomaly", false);
            os << "\nStatus: ";
            if (anom) {
                os << "ANOMALY DETECTED (severity: " << a.value("severity","?") << ", "
                   << a.value("reason","") << ")";
            } else {
                os << "NORMAL (values within " << a.value("label","")
                   << " range)";
            }
            os << "\n";
        }
        return os.str();
    }

    // Bilinmeyen tool -> JSON dump fallback
    return tr.dump(2);
}

// ============================================================
// CONFIG I/O
// ============================================================
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