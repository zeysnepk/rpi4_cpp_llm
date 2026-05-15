#pragma once
#include "sensor_manager.hpp"
#include <nlohmann/json.hpp>
#include <string>

class ToolDispatcher {
public:
    ToolDispatcher(SensorManager& sensors, std::string config_path);

    // OpenAI-uyumlu tool tanim listesi
    nlohmann::json get_tool_definitions() const;

    // Tool calistir, sonucu JSON olarak don
    nlohmann::json execute(const std::string& name, const nlohmann::json& args);

private:
    SensorManager& sensors_;
    std::string config_path_;

    nlohmann::json tool_get_current(const nlohmann::json& args);
    nlohmann::json tool_get_history_stats(const nlohmann::json& args);
    nlohmann::json tool_set_sample_rate(const nlohmann::json& args);

    nlohmann::json load_config();
    bool save_config(const nlohmann::json& cfg);
};