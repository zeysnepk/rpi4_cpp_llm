// Dashboard server: webui'yi sunar, /api/chat'i llama-server'a proxy'ler
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <chrono>
#include <fstream>

using json = nlohmann::json;

// --- Ayarlar ---
constexpr const char* DASHBOARD_HOST = "0.0.0.0";
constexpr int         DASHBOARD_PORT = 8081;
constexpr const char* LLAMA_HOST     = "localhost";
constexpr int         LLAMA_PORT     = 8080;
constexpr const char* WEBUI_DIR      = "./webui";
constexpr const char* CONFIG_PATH    = "./config.json";

// --- Yardimci: config dosyasini oku ---
static json load_config() {
    std::ifstream f(CONFIG_PATH);
    if (!f) return json::object();
    try { return json::parse(f); } catch (...) { return json::object(); }
}

int main() {
    httplib::Server server;

    // --- 1) Statik dosyalar (HTML/CSS/JS) ---
    if (!server.set_mount_point("/", WEBUI_DIR)) {
        std::cerr << "HATA: webui klasoru bulunamadi: " << WEBUI_DIR << "\n";
        return 1;
    }

    // --- 2) Saglik kontrolu ---
    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        json j = { {"status", "ok"}, {"llama_target", "localhost:8080"} };
        res.set_content(j.dump(), "application/json");
    });

    // --- 3) Config (simdilik salt-okunur, sonra LLM yazabilecek) ---
    server.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(load_config().dump(2), "application/json");
    });

    // --- 4) Chat proxy (SSE streaming) ---
    server.Post("/api/chat", [](const httplib::Request& req, httplib::Response& res) {
        json incoming;
        try {
            incoming = json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(std::string("Invalid JSON: ") + e.what(), "text/plain");
            return;
        }

        // llama-server icin OpenAI-uyumlu payload
        json payload = {
            {"model",        "local"},
            {"messages",     incoming.value("messages", json::array())},
            {"stream",       true},
            {"temperature",  incoming.value("temperature", 0.3)},
            {"top_p",        0.95},
            {"top_k",        40},
            {"max_tokens",   incoming.value("max_tokens", 512)},
            {"cache_prompt", true}
        };
        const std::string payload_str = payload.dump();

        // SSE response baslat
        res.set_chunked_content_provider(
            "text/event-stream",
            [payload_str](size_t /*offset*/, httplib::DataSink& sink) {
                httplib::Client cli(LLAMA_HOST, LLAMA_PORT);
                cli.set_read_timeout(std::chrono::seconds(600));
                cli.set_write_timeout(std::chrono::seconds(60));

                httplib::Headers headers = {
                    {"Accept", "text/event-stream"}
                };

                auto result = cli.Post(
                    "/v1/chat/completions",
                    headers,
                    payload_str,
                    "application/json",
                    // llama-server'dan gelen her chunk'i tarayiciya ilet
                    [&sink](const char* data, size_t len) -> bool {
                        return sink.write(data, len);
                    }
                );

                if (!result) {
                    std::string err = "data: {\"error\":\"llama-server unreachable on "
                                      "port 8080\"}\n\n";
                    sink.write(err.data(), err.size());
                }
                sink.done();
                return true;
            }
        );
    });

    // --- 5) CORS (gelistirme kolayligi icin) ---
    server.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    std::cout << "Dashboard server: http://" << DASHBOARD_HOST << ":"
              << DASHBOARD_PORT << "\n";
    std::cout << "llama-server hedef: " << LLAMA_HOST << ":" << LLAMA_PORT << "\n";
    std::cout << "Durdurmak icin Ctrl+C\n";

    server.listen(DASHBOARD_HOST, DASHBOARD_PORT);
    return 0;
}