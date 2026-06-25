#include "llm/intent_router.hpp"
#include <regex>
#include <string>
#include <cctype>
#include <unordered_map>

using json = nlohmann::json;

// ============================================================
// Turkish character normalization + lowercase
// ============================================================
static std::string normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    static const std::unordered_map<std::string, char> tr_map = {
        {"\xC4\xB1", 'i'}, {"\xC4\xB0", 'i'},  // ı, İ
        {"\xC3\xA7", 'c'}, {"\xC3\x87", 'c'},  // ç, Ç
        {"\xC4\x9F", 'g'}, {"\xC4\x9E", 'g'},  // ğ, Ğ
        {"\xC3\xB6", 'o'}, {"\xC3\x96", 'o'},  // ö, Ö
        {"\xC5\x9F", 's'}, {"\xC5\x9E", 's'},  // ş, Ş
        {"\xC3\xBC", 'u'}, {"\xC3\x9C", 'u'},  // ü, Ü
    };

    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out += (char)std::tolower(c);
            i++;
        } else if (c < 0xC0) {
            i++;
        } else if (c < 0xE0 && i + 1 < s.size()) {
            std::string ch = s.substr(i, 2);
            auto it = tr_map.find(ch);
            out += (it != tr_map.end()) ? it->second : ' ';
            i += 2;
        } else {
            i += (c < 0xF0) ? 3 : 4;
        }
    }
    return out;
}

// ============================================================
// Sensor adi tespiti
// ============================================================
static std::string detect_sensor(const std::string& norm) {
    if (norm.find("bme")      != std::string::npos ||
        norm.find("temp")     != std::string::npos ||
        norm.find("humid")    != std::string::npos ||
        norm.find("press")    != std::string::npos ||
        norm.find("room")     != std::string::npos ||
        norm.find("ambient")  != std::string::npos ||
        norm.find("weather")  != std::string::npos ||
        norm.find("atmosph")  != std::string::npos ||
        norm.find("baromet")  != std::string::npos) {
        return "bme280";
    }
    if (norm.find("mpu")      != std::string::npos ||
        norm.find("accel")    != std::string::npos ||
        norm.find("gyro")     != std::string::npos ||
        norm.find("vibrat")   != std::string::npos ||
        norm.find("motion")   != std::string::npos ||
        norm.find("tilt")     != std::string::npos ||
        norm.find("angular")  != std::string::npos ||
        norm.find("rotation") != std::string::npos) {
        return "mpu6050";
    }
    if (norm.find("qmc")       != std::string::npos ||
        norm.find("magnetic")  != std::string::npos ||
        norm.find("magnet")    != std::string::npos ||
        norm.find("heading")   != std::string::npos ||
        norm.find("compass")   != std::string::npos ||
        norm.find("direction") != std::string::npos ||
        norm.find("bearing")   != std::string::npos) {
        return "qmc5883l";
    }
    return "";
}

// ============================================================
// Metric tespiti
// ============================================================
static std::string detect_metric(const std::string& norm, const std::string& sensor) {
    auto has_axis = [&](char ax) -> bool {
        std::string p = " "; p += ax;
        return norm.find(p) != std::string::npos;
    };

    if (sensor == "bme280") {
        if (norm.find("humid")   != std::string::npos ||
            norm.find("moisture") != std::string::npos) return "humidity_pct";
        if (norm.find("press")   != std::string::npos ||
            norm.find("baromet") != std::string::npos ||
            norm.find("hpa")     != std::string::npos) return "pressure_hpa";
        return "temperature_c";
    }
    if (sensor == "mpu6050") {
        bool is_gyro = (norm.find("gyro")    != std::string::npos ||
                        norm.find("angular") != std::string::npos ||
                        norm.find("rotation") != std::string::npos);
        if (is_gyro) {
            if (has_axis('x')) return "gyro_dps.x";
            if (has_axis('y')) return "gyro_dps.y";
            return "gyro_dps.z";
        }
        if (has_axis('x')) return "accel_g.x";
        if (has_axis('y')) return "accel_g.y";
        return "accel_g.z";
    }
    if (sensor == "qmc5883l") {
        if (norm.find("heading")   != std::string::npos ||
            norm.find("direction") != std::string::npos ||
            norm.find("compass")   != std::string::npos ||
            norm.find("bearing")   != std::string::npos) return "heading_deg";
        if (has_axis('x')) return "mag_g.x";
        if (has_axis('y')) return "mag_g.y";
        if (has_axis('z')) return "mag_g.z";
        return "heading_deg";
    }
    return "";
}

// Interpretation/anomaly/evaluation request keywords
static bool has_interpret_keyword(const std::string& norm) {
    static const std::regex re(
        R"(anomal|interpret|evaluat|analyz|analyse|trend|good\s+or|bad\?|issue|problem|status|normal\?|fault|check\s+if|how\s+is|is\s+it\s+ok)",
        std::regex_constants::icase);
    return std::regex_search(norm, re);
}

// ============================================================
// PARSE
// ============================================================
IntentRouter::Intent IntentRouter::parse(const std::string& msg_tr,
                                          const std::string& last_sensor_hint) {
    std::string norm = normalize(msg_tr);

    // ----- 0a. GET CONFIG -----
    // Broad match: any phrasing around "what/which can be changed/configured/adjusted"
    {
        static const std::regex re(
            "show.*setting|list.*setting|show.*config|get.*config|"
            "what.*can.*chang|what.*can.*adjust|what.*can.*config|"
            "which.*can.*chang|which.*can.*adjust|which.*param|"
            "how.*do.*i.*chang|how.*can.*i.*chang|how.*to.*chang|how.*to.*set|"
            "i.*want.*to.*chang|want.*to.*adjust|can.*i.*chang|can.*you.*chang|"
            "list.*threshold|show.*threshold|list.*limit|show.*limit|"
            "show.*parameter|what.*parameter|which.*parameter|"
            "all.*setting|current.*config|print.*config|what.*setting|"
            "just.*it|is.*that.*all|that.*all|is.*everything|anything.*more.*change",
            std::regex_constants::icase);
        if (std::regex_search(norm, re)) {
            return {true, "get_config", json::object(), msg_tr};
        }
    }

    // ----- 0b. SET THRESHOLD — requires "set" keyword for safety -----
    {
        // Min/max direction detection
        bool has_lower = norm.find("lower")   != std::string::npos ||
                         norm.find("floor")   != std::string::npos ||
                         norm.find("bottom")  != std::string::npos;
        bool want_max = norm.find("max")    != std::string::npos ||
                        norm.find("upper")  != std::string::npos ||
                        norm.find("ceiling")!= std::string::npos ||
                        norm.find("high")   != std::string::npos;
        bool want_min = norm.find("min")    != std::string::npos ||
                        has_lower;

        // Threshold change verb
        static const std::regex thr_verb_re(
            R"(limit|threshold|boundary|ceiling|floor|upper|lower|set.*to|change.*to)",
            std::regex_constants::icase);
        bool is_threshold = std::regex_search(norm, thr_verb_re) || (want_max || want_min);

        // Numeric value
        static const std::regex val_re(R"((-?\d+(?:\.\d+)?))");
        std::smatch val_m;

        if (norm.find("set") != std::string::npos &&
            is_threshold && std::regex_search(norm, val_m, val_re)) {
            double val = std::stod(val_m[1].str());

            // Metric detection for thresholds (broader matching)
            std::string metric_key;
            if (norm.find("temp") != std::string::npos)
                metric_key = "bme280.temperature_c";
            else if (norm.find("humid") != std::string::npos || norm.find("moisture") != std::string::npos)
                metric_key = "bme280.humidity_pct";
            else if (norm.find("press") != std::string::npos || norm.find("baromet") != std::string::npos)
                metric_key = "bme280.pressure_hpa";
            else if (norm.find("mpu temp") != std::string::npos || norm.find("imu temp") != std::string::npos)
                metric_key = "mpu6050.temp_c";
            else if (norm.find("accel") != std::string::npos && norm.find(".x") != std::string::npos)
                metric_key = "mpu6050.accel_g.x";
            else if (norm.find("accel") != std::string::npos && norm.find(".y") != std::string::npos)
                metric_key = "mpu6050.accel_g.y";
            else if (norm.find("accel") != std::string::npos)
                metric_key = "mpu6050.accel_g.z";
            else if (norm.find("gyro") != std::string::npos)
                metric_key = "mpu6050.gyro_dps.x";
            else if (norm.find("heading") != std::string::npos || norm.find("compass") != std::string::npos)
                metric_key = "qmc5883l.heading_deg";

            // Fallback: sensor name without metric — default to most common metric
            if (metric_key.empty()) {
                if (norm.find("bme") != std::string::npos)
                    metric_key = "bme280.temperature_c";
                else if (norm.find("mpu") != std::string::npos)
                    metric_key = "mpu6050.temp_c";
                else if (norm.find("qmc") != std::string::npos)
                    metric_key = "qmc5883l.heading_deg";
            }

            if (!metric_key.empty()) {
                json args = {{"metric", metric_key}};
                // If only max or only min keyword matched, set that side;
                // if both or neither matched, apply symmetrically to min and max.
                if (want_max && !want_min)       args["max"] = val;
                else if (want_min && !want_max)  args["min"] = val;
                else {
                    // e.g. "temperature threshold 35" → default to max=35
                    args["max"] = val;
                }
                return {true, "set_threshold", args, msg_tr};
            }
        }
    }

    // ----- 0c. SET SENSOR ENABLED ("disable bme280", "turn on mpu") -----
    {
        static const std::regex disable_re(
            R"(\bdisabled?\b|\bturn\s*off\b|\bstop\b|\bdeactivate\b|\bswitch\s*off\b)",
            std::regex_constants::icase);
        static const std::regex enable_re(
            R"(\benabled?\b|\bturn\s*on\b|\bstart\b|\bactivate\b|\bswitch\s*on\b)",
            std::regex_constants::icase);
        bool want_disable = std::regex_search(norm, disable_re);
        bool want_enable  = std::regex_search(norm, enable_re);

        if ((want_disable || want_enable) && norm.find("set") != std::string::npos) {
            std::string sensor_en;
            if (norm.find("bme") != std::string::npos)       sensor_en = "bme280";
            else if (norm.find("mpu") != std::string::npos)  sensor_en = "mpu6050";
            else if (norm.find("qmc") != std::string::npos)  sensor_en = "qmc5883l";

            if (!sensor_en.empty()) {
                return {true, "set_sensor_enabled",
                        {{"sensor", sensor_en}, {"enabled", !want_disable}}, msg_tr};
            }
        }
    }

    // ----- 1a. SET SAMPLE RATE (sensor + number + hz/frequency) -----
    // "mpu 20hz", "bme sample rate 10", "qmc 50 hertz"
    {
        static const std::regex re(
            R"((bme280|bme|mpu6500|mpu6050|mpu|qmc5883l?|qmc).{0,30}?(\d+)\s*(?:h(?:z|ertz))?)",
            std::regex_constants::icase);
        static const std::regex rate_kw_re(
            R"(hz|hertz|frequency|sampling|sample.?rate)",
            std::regex_constants::icase);
        std::smatch m;
        if (norm.find("set") != std::string::npos &&
            std::regex_search(norm, m, re) && std::regex_search(norm, rate_kw_re)) {
            std::string raw = m[1].str();
            int hz = std::stoi(m[2].str());
            std::string sensor;
            if (raw.find("bme") != std::string::npos) sensor = "bme280";
            else if (raw.find("mpu") != std::string::npos) sensor = "mpu6050";
            else if (raw.find("qmc") != std::string::npos) sensor = "qmc5883l";
            return {true, "set_sample_rate",
                    {{"sensor", sensor}, {"hz", hz}}, msg_tr};
        }
    }

    // ----- 1b. SET SAMPLE RATE (number + hz only, sensor from context) -----
    {
        static const std::regex re(R"((\d+)\s*h(?:z|ertz))", std::regex_constants::icase);
        std::smatch m;
        if (norm.find("set") != std::string::npos && std::regex_search(norm, m, re)) {
            int hz = std::stoi(m[1].str());
            std::string sensor = last_sensor_hint.empty() ? "bme280" : last_sensor_hint;
            return {true, "set_sample_rate",
                    {{"sensor", sensor}, {"hz", hz}}, msg_tr};
        }
    }

    // ----- 1c. GET HISTORY RAW (last N readings/values/samples) -----
    // Must come before HISTORY STATS to catch count-based queries first.
    {
        static const std::regex re(
            R"(last\s+(\d+)\s*(?:\w+\s+)?(?:reading|value|sample|data\s*point|measurement|entr(?:y|ies)|data)s?)",
            std::regex_constants::icase);
        std::smatch m;
        if (std::regex_search(norm, m, re)) {
            int count = std::stoi(m[1].str());
            if (count > 100) count = 100;

            bool want_all = norm.find("all")   != std::string::npos ||
                            norm.find("each")  != std::string::npos ||
                            norm.find("every") != std::string::npos;

            std::string sensor;
            if (want_all) {
                sensor = "all";
            } else {
                sensor = detect_sensor(norm);
                if (sensor.empty())
                    sensor = last_sensor_hint.empty() ? "bme280" : last_sensor_hint;
            }
            return {true, "get_history_raw",
                    {{"sensor", sensor}, {"count", count}},
                    msg_tr};
        }
    }

    // ----- 2. HISTORY STATS (time window: "last X seconds / minutes") -----
    {
        std::regex re(R"(last\s+(\d+)\s*(sec(?:ond)?s?|min(?:ute)?s?))",
                      std::regex_constants::icase);
        std::smatch m;
        if (std::regex_search(norm, m, re)) {
            int n = std::stoi(m[1].str());
            std::string unit = m[2].str();
            int seconds = n;
            if (unit.rfind("min", 0) == 0) seconds = n * 60;

            std::string sensor = detect_sensor(norm);
            if (sensor.empty()) {
                sensor = last_sensor_hint.empty() ? "bme280" : last_sensor_hint;
            }
            std::string metric = detect_metric(norm, sensor);

            return {true, "get_history_stats",
                    {{"sensor", sensor}, {"metric", metric}, {"seconds", seconds}},
                    msg_tr};
        }
    }

    // ----- 3. ALL SENSORS -----
    {
        std::regex re(R"(\ball\b.*sensor|every.*sensor|sensor.*all|show.*all|read.*all)",
                      std::regex_constants::icase);
        if (std::regex_search(norm, re)) {
            return {true, "get_current", {{"sensor", "all"}}, msg_tr};
        }
    }

    // ----- 4. SENSOR QUERY + (opsiyonel) yorum talep -----
    // Sensor adi varsa: anlik mi yoksa gecmis mi karar ver
    std::string sensor = detect_sensor(norm);
    if (!sensor.empty()) {
        if (has_interpret_keyword(norm)) {
            // Yorum istemi -> son 60 sn istatistigi (trend gerekir)
            std::string metric = detect_metric(norm, sensor);
            return {true, "get_history_stats",
                    {{"sensor", sensor}, {"metric", metric}, {"seconds", 60}},
                    msg_tr};
        }
        return {true, "get_current", {{"sensor", sensor}}, msg_tr};
    }

    // ----- 5. INTERPRET ONLY (no sensor name, but an interpretation request) -----
    // e.g. "is there an anomaly", "what's the status", "is anything wrong"
    if (has_interpret_keyword(norm)) {
        std::string s = last_sensor_hint.empty() ? "bme280" : last_sensor_hint;
        std::string metric = detect_metric(norm, s);
        return {true, "get_history_stats",
                {{"sensor", s}, {"metric", metric}, {"seconds", 60}},
                msg_tr};
    }

    // ----- 6. NO MATCH -> LLM SOHBET -----
    return {false, "", json::object(), msg_tr};
}