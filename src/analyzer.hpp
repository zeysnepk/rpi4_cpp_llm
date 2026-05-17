#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Domain-spesifik analiz: anomali tespiti, normal/yuksek/dusuk siniflandirma.
// Config'deki "thresholds" bolumunden okur.
//
// Kullanim:
//   Analyzer az(cfg);
//   auto result = az.analyze("bme280", "temperature_c", 26.5);
//   // result = {"status":"normal", "label":"oda sicakligi", "anomaly":false, ...}
class Analyzer {
public:
    explicit Analyzer(const nlohmann::json& config);

    // Bir tek deger icin durum analizi
    nlohmann::json analyze(const std::string& sensor,
                           const std::string& metric,
                           double value) const;

    // History stats icin: hem anlik (last) hem min/max anomalisi
    nlohmann::json analyze_stats(const std::string& sensor,
                                  const std::string& metric,
                                  double min_v, double max_v,
                                  double avg_v, double last_v) const;

private:
    nlohmann::json thresholds_;  // sensor.metric -> {min, max, label}

    // "bme280.temperature_c" key'i icin esik bul; yoksa nullopt
    const nlohmann::json* get_threshold(const std::string& sensor,
                                         const std::string& metric) const;
};