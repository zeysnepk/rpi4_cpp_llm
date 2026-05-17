#pragma once
#include <string>
#include <atomic>
#include <nlohmann/json.hpp>

// LibreTranslate ile TR<->EN ceviri katmani.
// Devre disi veya servis erisilemez ise orijinal text doner (graceful fallback).
class Translator {
public:
    explicit Translator(const nlohmann::json& cfg);

    bool enabled()   const { return enabled_; }
    bool available() const { return available_.load(); }

    // /languages endpoint'i ile health check; available_ flag'ini gunceller.
    bool refresh_status();

    // Hata veya devre disi -> orijinal text doner (caller'a transparan).
    std::string tr_to_en(const std::string& text);
    std::string en_to_tr(const std::string& text);

private:
    std::string translate(const std::string& text,
                          const std::string& source,
                          const std::string& target);

    bool        enabled_;
    std::string host_;
    int         port_;
    int         timeout_ms_;
    std::atomic<bool> available_{false};
};