#include "translator.hpp"
#include <httplib.h>
#include <iostream>

using json = nlohmann::json;

Translator::Translator(const json& cfg) {
    auto t = cfg.value("translator", json::object());
    enabled_    = t.value("enabled", false);
    host_       = t.value("host", std::string("localhost"));
    port_       = t.value("port", 5000);
    timeout_ms_ = t.value("timeout_ms", 8000);

    if (enabled_) {
        std::cout << "Translator: " << host_ << ":" << port_;
        if (refresh_status()) std::cout << " [OK]\n";
        else                  std::cout << " [ERISILEMEZ - fallback aktif]\n";
    } else {
        std::cout << "Translator: devre disi (LLM dogrudan TR)\n";
    }
}

bool Translator::refresh_status() {
    if (!enabled_) { available_ = false; return false; }

    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(1, 0);

    auto res = cli.Get("/languages");
    bool ok = (res && res->status == 200);
    available_ = ok;
    return ok;
}

std::string Translator::translate(const std::string& text,
                                   const std::string& source,
                                   const std::string& target) {
    if (!enabled_ || text.empty()) return text;

    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(timeout_ms_ / 1000, (timeout_ms_ % 1000) * 1000);

    json body = {
        {"q",      text},
        {"source", source},
        {"target", target},
        {"format", "text"}
    };

    auto res = cli.Post("/translate", body.dump(), "application/json");
    if (!res || res->status != 200) {
        std::cerr << "Translator hata (" << source << "->" << target << "): "
                  << (res ? std::to_string(res->status)
                          : std::string("baglanti yok")) << "\n";
        available_ = false;
        return text;  // graceful fallback
    }

    try {
        auto result = json::parse(res->body);
        available_ = true;
        return result.value("translatedText", text);
    } catch (const std::exception& e) {
        std::cerr << "Translator parse hatasi: " << e.what() << "\n";
        return text;
    }
}

std::string Translator::tr_to_en(const std::string& text) {
    return translate(text, "tr", "en");
}

std::string Translator::en_to_tr(const std::string& text) {
    return translate(text, "en", "tr");
}