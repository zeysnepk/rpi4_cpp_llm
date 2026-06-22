#include "llm/analyzer.hpp"
#include <iostream>

using json = nlohmann::json;

Analyzer::Analyzer(const json& config) {
    thresholds_ = config.value("thresholds", json::object()); // .value(key, default)
    if (thresholds_.empty()) {
        std::cout << "Analyzer: no thresholds found in config\n";
    } else {
        std::cout << "Analyzer: " << thresholds_.size()
                  << " thresholds loaded\n";
    }
}

const json* Analyzer::get_threshold(const std::string& sensor, // * -> adres doner, yok ise nullptr
                                     const std::string& metric) const {
    std::string key = sensor + "." + metric;
    if (thresholds_.contains(key)) return &thresholds_[key]; // key varsa adresini don
    return nullptr;
}

json Analyzer::analyze(const std::string& sensor,
                       const std::string& metric,
                       double value) const { // const -> sadece okunabilir, degistirilemez
    const json* th = get_threshold(sensor, metric);
    if (!th) {
        return json::object();  // no threshold defined — skip analysis
    }

    double tmin = th->value("min", -1e9);
    double tmax = th->value("max",  1e9);
    std::string label = th->value("label", std::string(""));

    std::string status;
    if (value < tmin)      status = "low";
    else if (value > tmax) status = "high";
    else                   status = "normal";

    bool anomaly = (status != "normal");

    return {
        {"status",  status},
        {"label",   label},
        {"anomaly", anomaly},
        {"expected_range", { {"min", tmin}, {"max", tmax} }}
    };
}

json Analyzer::analyze_stats(const std::string& sensor,
                              const std::string& metric,
                              double min_v, double max_v,
                              double avg_v, double last_v) const {
    const json* th = get_threshold(sensor, metric);
    if (!th) return json::object();

    double tmin = th->value("min", -1e9);
    double tmax = th->value("max",  1e9);
    std::string label = th->value("label", std::string(""));

    // Check if any min/max threshold was breached
    bool min_out = (min_v < tmin);
    bool max_out = (max_v > tmax);
    bool any_anomaly = min_out || max_out;

    std::string severity = "none";
    std::string reason;
    if (any_anomaly) {
        // Compute breach magnitude as % of configured range
        double margin = 0;
        if (max_out) margin = max_v - tmax;
        if (min_out) margin = std::max(margin, tmin - min_v);

        double range = tmax - tmin;
        double margin_pct = (range > 0) ? (margin / range) * 100.0 : 0;

        if (margin_pct < 10)       severity = "low";
        else if (margin_pct < 30)  severity = "medium";
        else                       severity = "high";

        if (max_out && min_out)
            reason = "both min=" + std::to_string(min_v) + " and max=" +
                     std::to_string(max_v) + " out of range";
        else if (max_out)
            reason = "max=" + std::to_string(max_v) +
                     " > threshold " + std::to_string(tmax);
        else
            reason = "min=" + std::to_string(min_v) +
                     " < threshold " + std::to_string(tmin);
    }

    // Status of the most recent (last) reading
    std::string last_status;
    if (last_v < tmin)      last_status = "low";
    else if (last_v > tmax) last_status = "high";
    else                    last_status = "normal";

    return {
        {"label",          label},
        {"expected_range", { {"min", tmin}, {"max", tmax} }},
        {"current_status", last_status},
        {"anomaly",        any_anomaly},
        {"severity",       severity},
        {"reason",         reason}
    };
}