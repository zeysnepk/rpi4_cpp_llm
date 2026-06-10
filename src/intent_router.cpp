#include "intent_router.hpp"
#include <regex>
#include <string>
#include <cctype>
#include <unordered_map>

using json = nlohmann::json;

// ============================================================
// Turkce karakter sadelestirme + lowercase
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
    if (norm.find("bme")     != std::string::npos ||
        norm.find("sicak")   != std::string::npos ||
        norm.find("temp")    != std::string::npos ||
        norm.find("nem")     != std::string::npos ||
        norm.find("humid")   != std::string::npos ||
        norm.find("basinc")  != std::string::npos ||
        norm.find("press")   != std::string::npos ||
        norm.find("hava")    != std::string::npos ||
        norm.find("oda")     != std::string::npos ||
        norm.find("ortam")   != std::string::npos ||
        norm.find("cevre")   != std::string::npos) {
        return "bme280";
    }
    if (norm.find("mpu")     != std::string::npos ||
        norm.find("ivme")    != std::string::npos ||
        norm.find("accel")   != std::string::npos ||
        norm.find("gyro")    != std::string::npos ||
        norm.find("jiros")   != std::string::npos ||
        norm.find("acisal")  != std::string::npos ||
        norm.find("titres")  != std::string::npos ||
        norm.find("vibrat")  != std::string::npos) {
        return "mpu6050";
    }
    if (norm.find("qmc")        != std::string::npos ||
        norm.find("manyetik")   != std::string::npos ||
        norm.find("magnetic")   != std::string::npos ||
        norm.find("heading")    != std::string::npos ||
        norm.find("pusula")     != std::string::npos ||
        norm.find(" yon")       != std::string::npos ||
        norm.find("yonel")      != std::string::npos ||
        norm.find("compass")    != std::string::npos) {
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
        if (norm.find("nem")    != std::string::npos ||
            norm.find("humid")  != std::string::npos) return "humidity_pct";
        if (norm.find("basinc") != std::string::npos ||
            norm.find("press")  != std::string::npos) return "pressure_hpa";
        return "temperature_c";
    }
    if (sensor == "mpu6050") {
        bool is_gyro = (norm.find("gyro")   != std::string::npos ||
                        norm.find("jiros")  != std::string::npos ||
                        norm.find("acisal") != std::string::npos);
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
        if (norm.find("heading") != std::string::npos ||
            norm.find("yon")     != std::string::npos ||
            norm.find("pusula")  != std::string::npos) return "heading_deg";
        if (has_axis('x')) return "mag_g.x";
        if (has_axis('y')) return "mag_g.y";
        if (has_axis('z')) return "mag_g.z";
        return "heading_deg";
    }
    return "";
}

// Yorum/anomali/degerlendirme talep keyword'leri
static bool has_interpret_keyword(const std::string& norm) {
    static const std::regex re(
        R"(anomali|yorum|degerlendir|analiz|trend|iyi\s+mi|kotu\s+mu|sorun|problem|durum|normal\s+mi|hata)",
        std::regex_constants::icase);
    return std::regex_search(norm, re);
}

// ============================================================
// PARSE
// ============================================================
IntentRouter::Intent IntentRouter::parse(const std::string& msg_tr,
                                          const std::string& last_sensor_hint) {
    std::string norm = normalize(msg_tr);

    // ----- 0a. GET CONFIG ("hangi değerleri değiştirebilirim", "tüm ayarlar") -----
    {
        static const std::regex re(
            "hangi.*degis|ne.*degistirebil|ayarlar.*goster|tum.*ayar|"
            "konfigurasyon|config.*goster|parametreler|esik.*listesi|"
            "limit.*listesi|ne.*ayarlayabilirim|ne.*yapilandir",
            std::regex_constants::icase);
        if (std::regex_search(norm, re)) {
            return {true, "get_config", json::object(), msg_tr};
        }
    }

    // ----- 0b. SET THRESHOLD ("sıcaklık max 35 yap", "nem üst sınırını 70 yap") -----
    {
        // Önce min/max yön tespiti
        bool want_max = norm.find("max")    != std::string::npos ||
                        norm.find("ust")    != std::string::npos ||
                        norm.find("tavan")  != std::string::npos ||
                        norm.find("en faz") != std::string::npos ||
                        norm.find("sinir")  != std::string::npos;
        bool want_min = norm.find("min")    != std::string::npos ||
                        norm.find("alt")    != std::string::npos ||
                        norm.find("zemin")  != std::string::npos ||
                        norm.find("en az")  != std::string::npos ||
                        norm.find("asagi")  != std::string::npos;

        // Eşik değiştirme fiili
        static const std::regex thr_verb_re(
            R"(esik|limit|sinir|tavan|zemin|threshold|esigi|limiti|siniri)",
            std::regex_constants::icase);
        bool is_threshold = std::regex_search(norm, thr_verb_re) || (want_max || want_min);

        // Sayısal değer
        static const std::regex val_re(R"((-?\d+(?:\.\d+)?))");
        std::smatch val_m;

        if (is_threshold && std::regex_search(norm, val_m, val_re)) {
            double val = std::stod(val_m[1].str());

            // Metric tespiti (esik için daha geniş eşleşme)
            std::string metric_key;
            if (norm.find("sicak") != std::string::npos || norm.find("temp") != std::string::npos)
                metric_key = "bme280.temperature_c";
            else if (norm.find("nem") != std::string::npos || norm.find("humid") != std::string::npos)
                metric_key = "bme280.humidity_pct";
            else if (norm.find("basinc") != std::string::npos || norm.find("press") != std::string::npos)
                metric_key = "bme280.pressure_hpa";
            else if (norm.find("mpu sicak") != std::string::npos || norm.find("islemci") != std::string::npos)
                metric_key = "mpu6050.temp_c";
            else if ((norm.find("ivme") != std::string::npos || norm.find("accel") != std::string::npos) &&
                      norm.find(".x") != std::string::npos)
                metric_key = "mpu6050.accel_g.x";
            else if ((norm.find("ivme") != std::string::npos || norm.find("accel") != std::string::npos) &&
                      norm.find(".y") != std::string::npos)
                metric_key = "mpu6050.accel_g.y";
            else if ((norm.find("ivme") != std::string::npos || norm.find("accel") != std::string::npos))
                metric_key = "mpu6050.accel_g.z";
            else if (norm.find("gyro") != std::string::npos || norm.find("jiro") != std::string::npos)
                metric_key = "mpu6050.gyro_dps.x";
            else if (norm.find("heading") != std::string::npos || norm.find("pusula") != std::string::npos)
                metric_key = "qmc5883l.heading_deg";

            if (!metric_key.empty()) {
                json args = {{"metric", metric_key}};
                // Eğer sadece max ya da min keyword'ü varsa onu set et;
                // ikisi de varsa veya hiçbiri yoksa hem min hem max'ı simetrik set et.
                if (want_max && !want_min)       args["max"] = val;
                else if (want_min && !want_max)  args["min"] = val;
                else {
                    // "sıcaklık eşiği 35 yap" → max=35 varsayılan
                    args["max"] = val;
                }
                return {true, "set_threshold", args, msg_tr};
            }
        }
    }

    // ----- 0c. SET SENSOR ENABLED ("bme280 kapat", "mpu aç") -----
    {
        static const std::regex disable_re(
            R"(\bkapat\b|\bdurdur\b|\bdisable\b|\bdevre\s*dis\b)",
            std::regex_constants::icase);
        static const std::regex enable_re(
            R"(\bac\b|\bacik\b|\bbasla\b|\benable\b|\baktif\s*et\b|\bdevreye\s*al\b)",
            std::regex_constants::icase);
        bool want_disable = std::regex_search(norm, disable_re);
        bool want_enable  = std::regex_search(norm, enable_re);

        if (want_disable || want_enable) {
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

    // ----- 1a. SET SAMPLE RATE (sensor + hz) -----
    {
        std::regex re(
            R"((bme280|bme|mpu6500|mpu6050|mpu|qmc5883l?|qmc).{0,30}?(\d+)\s*h(?:z|ertz))",
            std::regex_constants::icase);
        std::smatch m;
        if (std::regex_search(norm, m, re)) {
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

    // ----- 1b. SET SAMPLE RATE (sadece hz, context'ten sensor) -----
    {
        std::regex re(R"((\d+)\s*h(?:z|ertz))", std::regex_constants::icase);
        std::smatch m;
        if (std::regex_search(norm, m, re)) {
            int hz = std::stoi(m[1].str());
            std::string sensor = last_sensor_hint.empty() ? "bme280" : last_sensor_hint;
            return {true, "set_sample_rate",
                    {{"sensor", sensor}, {"hz", hz}}, msg_tr};
        }
    }

    // ----- 2. HISTORY STATS (acik zaman penceresi: "son X saniye") -----
    {
        std::regex re(R"(son\s+(\d+)\s+(saniye|sn|dakika|dk|min|minute))",
                      std::regex_constants::icase);
        std::smatch m;
        if (std::regex_search(norm, m, re)) {
            int n = std::stoi(m[1].str());
            std::string unit = m[2].str();
            int seconds = n;
            if (unit == "dakika" || unit == "dk" ||
                unit == "min" || unit == "minute") seconds = n * 60;

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
        std::regex re(R"(\btum\b|hepsi|butun|all\s*sensor)",
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

    // ----- 5. INTERPRET ONLY (sensor adi yok ama yorum istemi var) -----
    // "anomali var mi yorumla", "durum nasil", "sorun var mi"
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