#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Parses a user message using regex/keyword matching to identify intent and tool call.
// last_sensor_hint: if the message omits a sensor name, the hint provides context
// (e.g. for follow-up queries like "set it to 20 Hz").
class IntentRouter {
public:
    struct Intent {
        bool             is_tool;
        std::string      tool_name;
        nlohmann::json   args;
        std::string      original;
    };

    static Intent parse(const std::string& message_tr,
                        const std::string& last_sensor_hint = "");
};