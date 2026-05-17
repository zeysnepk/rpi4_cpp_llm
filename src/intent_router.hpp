#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Kullanici Turkce mesajini regex/keyword ile parse eder.
// last_sensor_hint: mesajda sensor adi geciyorsa onu kullanir,
// gecmiyorsa hint'i kullanir ("Simdi 20 Hz yap" gibi context-bagimli sorgular icin).
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