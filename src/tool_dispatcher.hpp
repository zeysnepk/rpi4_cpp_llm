#pragma once
#include "sensor_manager.hpp"
#include "analyzer.hpp"
#include <nlohmann/json.hpp>
#include <string>

class ToolDispatcher {
public:
    ToolDispatcher(SensorManager& sensors,
                   const Analyzer& analyzer,
                   std::string config_path);

    nlohmann::json get_tool_definitions() const;
    nlohmann::json execute(const std::string& name, const nlohmann::json& args);

    // LLM'e gidecek tool sonucunu Turkce metin olarak formatla
    // (yuvarlanmis, anlamli, kisa)
    std::string format_for_llm(const std::string& tool_name,
                                const nlohmann::json& tool_result) const;

private:
    SensorManager& sensors_;
    const Analyzer& analyzer_;
    std::string config_path_;

    nlohmann::json tool_get_current(const nlohmann::json& args);
    nlohmann::json tool_get_history_stats(const nlohmann::json& args);
    nlohmann::json tool_set_sample_rate(const nlohmann::json& args);
    nlohmann::json tool_set_threshold(const nlohmann::json& args);
    nlohmann::json tool_set_sensor_enabled(const nlohmann::json& args);
    nlohmann::json tool_get_config(const nlohmann::json& args);

    nlohmann::json load_config();
    bool save_config(const nlohmann::json& cfg);
};