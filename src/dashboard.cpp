#include <httplib.h>
#include <nlohmann/json.hpp>
#include "sensor_manager.hpp"

#include <iostream>
#include <fstream>
#include <chrono>
#include <atomic>
#include <csignal>

using json = nlohmann::json;

constexpr const char* DASHBOARD_HOST = "0.0.0.0";
constexpr int         DASHBOARD_PORT = 8081;
constexpr const char* LLAMA_HOST     = "localhost";
constexpr int         LLAMA_PORT     = 8080;
constexpr const char* WEBUI_DIR      = "./webui";
constexpr const char* CONFIG_PATH    = "./config.json";

static json load_config() {
    std::ifstream f(CONFIG_PATH);
    if (!f) return json::object();
    try { return json::parse(f); } catch (...) { return json::object(); }
}

// Server'i gracefully kapatmak icin global pointer
static httplib::Server* g_server = nullptr;
static void on_signal(int) { if (g_server) g_server->stop(); }

int main() {
    json cfg = load_config();
    std::cout << "Config yuklendi: " << CONFIG_PATH << "\n";

    SensorManager sensors(cfg);
    sensors.start();

    httplib::Server server;
    g_server = &server;
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    if (!server.set_mount_point("/", WEBUI_DIR)) {
        std::cerr << "HATA: webui klasoru yok\n";
        return 1;
    }

    // CORS
    server.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"status","ok"}}.dump(), "application/json");
    });

    server.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(load_config().dump(2), "application/json");
    });

    // SENSOR endpoint'leri
    server.Get("/api/sensors", [&sensors](const httplib::Request&, httplib::Response& res) {
        res.set_content(sensors.latest_all().dump(), "application/json");
    });

    server.Get("/api/sensors/history", [&sensors](const httplib::Request& req,
                                                  httplib::Response& res) {
        std::string name = req.get_param_value("sensor");
        int seconds = 60;
        if (req.has_param("seconds")) seconds = std::stoi(req.get_param_value("seconds"));
        res.set_content(sensors.history(name, seconds).dump(), "application/json");
    });

    // CHAT proxy (degismedi)
    server.Post("/api/chat", [](const httplib::Request& req, httplib::Response& res) {
        json incoming;
        try { incoming = json::parse(req.body); }
        catch (const std::exception& e) {
            res.status = 400;
            res.set_content(std::string("Invalid JSON: ") + e.what(), "text/plain");
            return;
        }
        json payload = {
            {"model","local"},
            {"messages",     incoming.value("messages", json::array())},
            {"stream",       true},
            {"temperature",  incoming.value("temperature", 0.3)},
            {"top_p", 0.95}, {"top_k", 40},
            {"max_tokens",   incoming.value("max_tokens", 512)},
            {"cache_prompt", true}
        };
        const std::string payload_str = payload.dump();

        res.set_chunked_content_provider("text/event-stream",
            [payload_str](size_t, httplib::DataSink& sink) -> bool {
                httplib::Client cli(LLAMA_HOST, LLAMA_PORT);
                cli.set_read_timeout(std::chrono::seconds(600));
                cli.set_write_timeout(std::chrono::seconds(60));

                // Manuel Request: streaming response icin tek temiz yol
                httplib::Request rq;
                rq.method = "POST";
                rq.path   = "/v1/chat/completions";
                rq.headers.emplace("Content-Type", "application/json");
                rq.headers.emplace("Accept",       "text/event-stream");
                rq.body   = payload_str;

                // llama-server'dan gelen her chunk'i tarayiciya ilet
                rq.content_receiver = [&sink](const char* data, size_t len,
                                              uint64_t /*off*/, uint64_t /*tot*/) -> bool {
                    return sink.write(data, len);
                };

                auto result = cli.send(rq);
                if (!result) {
                    std::string err = "data: {\"error\":\"llama-server unreachable on "
                                      "port 8080\"}\n\n";
                    sink.write(err.data(), err.size());
                }
                sink.done();
                return true;
            });
    });

    std::cout << "Dashboard: http://" << DASHBOARD_HOST << ":" << DASHBOARD_PORT << "\n";
    server.listen(DASHBOARD_HOST, DASHBOARD_PORT);
    sensors.stop();
    return 0;
}