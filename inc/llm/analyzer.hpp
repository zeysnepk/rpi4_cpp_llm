#pragma once  // include guard
#include <string>
#include <nlohmann/json.hpp>

// Domain-specific analysis: anomaly detection, normal/high/low classification.
// Reads the "thresholds" section from the config JSON.
//
// Usage:
//   Analyzer az(cfg);
//   auto result = az.analyze("bme280", "temperature_c", 26.5);
//   // result = {"status":"normal", "label":"room temperature", "anomaly":false, ...}
class Analyzer {
public:
    explicit Analyzer(const nlohmann::json& config);

    // Analyze a single point value against configured thresholds
    nlohmann::json analyze(const std::string& sensor,
                           const std::string& metric,
                           double value) const;

    // Analyze history stats: instant (last) status + min/max breach detection
    nlohmann::json analyze_stats(const std::string& sensor,
                                  const std::string& metric,
                                  double min_v, double max_v,
                                  double avg_v, double last_v) const;

private:
    nlohmann::json thresholds_;  // sensor.metric -> {min, max, label}

    // Look up threshold for key "bme280.temperature_c"; returns nullptr if absent
    const nlohmann::json* get_threshold(const std::string& sensor,
                                         const std::string& metric) const;
};