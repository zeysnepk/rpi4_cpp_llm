#include "tool_dispatcher.hpp"
#include "sensor_manager.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <csignal>

using json = nlohmann::json;

// ============================================================
// AYARLAR
// ============================================================
constexpr const char* DASHBOARD_HOST = "0.0.0.0";
constexpr int         DASHBOARD_PORT = 8081;
constexpr const char* LLAMA_HOST     = "localhost";
constexpr int         LLAMA_PORT     = 8080;
constexpr const char* WEBUI_DIR      = "./webui";
constexpr const char* CONFIG_PATH    = "./config.json";

// ============================================================
// YARDIMCILAR
// ============================================================
static json load_config() {
    std::ifstream f(CONFIG_PATH);
    if (!f) return json::object();
    try { return json::parse(f); } catch (...) { return json::object(); }
}

// Graceful shutdown icin global pointer
static httplib::Server* g_server = nullptr;
static void on_signal(int) {
    if (g_server) g_server->stop();
}

// ============================================================
// MAIN
// ============================================================
int main() {
    json cfg = load_config();
    std::cout << "Config yuklendi: " << CONFIG_PATH << "\n";

    // --- Sensor yonetimi ---
    SensorManager sensors(cfg);
    sensors.start();

    // --- LLM tool dispatcher ---
    ToolDispatcher dispatcher(sensors, CONFIG_PATH);

    // --- HTTP server ---
    httplib::Server server;
    g_server = &server;
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // Static dosyalar (webui klasoru)
    if (!server.set_mount_point("/", WEBUI_DIR)) {
        std::cerr << "HATA: webui klasoru bulunamadi: " << WEBUI_DIR << "\n";
        sensors.stop();
        return 1;
    }

    // CORS (gelistirme kolayligi icin)
    server.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ============================================
    // BASIT ENDPOINT'LER
    // ============================================

    // Saglik kontrolu
    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"status", "ok"}}.dump(), "application/json");
    });

    // Config oku (salt-okunur)
    server.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(load_config().dump(2), "application/json");
    });

    // ============================================
    // SENSOR ENDPOINT'LERI
    // ============================================

    // Anlik snapshot (tek istek)
    server.Get("/api/sensors", [&sensors](const httplib::Request&, httplib::Response& res) {
        res.set_content(sensors.latest_all().dump(), "application/json");
    });

    // Gecmis veri (chart icin)
    server.Get("/api/sensors/history", [&sensors](const httplib::Request& req,
                                                  httplib::Response& res) {
        std::string name = req.get_param_value("sensor");
        int seconds = 60;
        if (req.has_param("seconds")) {
            try { seconds = std::stoi(req.get_param_value("seconds")); }
            catch (...) { seconds = 60; }
        }
        res.set_content(sensors.history(name, seconds).dump(), "application/json");
    });

    // SSE push (5 Hz canli akis)
    server.Get("/api/sensors/stream", [&sensors](const httplib::Request&,
                                                 httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");

        res.set_chunked_content_provider("text/event-stream",
            [&sensors](size_t, httplib::DataSink& sink) -> bool {
                // Retry hint
                std::string hello = ": connected\nretry: 2000\n\n";
                if (!sink.write(hello.data(), hello.size())) return true;

                // Surekli push - client disconnect ettiginde is_writable false doner
                while (sink.is_writable()) {
                    auto data = sensors.latest_all();
                    std::string msg = "data: " + data.dump() + "\n\n";
                    if (!sink.write(msg.data(), msg.size())) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 5 Hz
                }
                sink.done();
                return true;
            });
    });

    // ============================================
    // CHAT ENDPOINT (LLM + tool calling)
    // ============================================
    server.Post("/api/chat", [&dispatcher](const httplib::Request& req,
                                           httplib::Response& res) {
        json incoming;
        try { incoming = json::parse(req.body); }
        catch (const std::exception& e) {
            res.status = 400;
            res.set_content(std::string("Invalid JSON: ") + e.what(), "text/plain");
            return;
        }

        res.set_chunked_content_provider("text/event-stream",
            [incoming, &dispatcher](size_t, httplib::DataSink& sink) -> bool {

                auto send_event = [&sink](const json& ev) -> bool {
                    std::string s = "data: " + ev.dump() + "\n\n";
                    return sink.write(s.data(), s.size());
                };

                json messages = incoming.value("messages", json::array());
                json tools    = dispatcher.get_tool_definitions();
                const int MAX_ITER = 5;

                for (int iter = 0; iter < MAX_ITER; iter++) {
                    json payload = {
                        {"model",        "local"},
                        {"messages",     messages},
                        {"tools",        tools},
                        {"tool_choice",  "auto"},
                        {"stream",       false},
                        {"temperature",  incoming.value("temperature", 0.3)},
                        {"max_tokens",   incoming.value("max_tokens", 512)},
                        {"cache_prompt", true}
                    };

                    httplib::Client cli(LLAMA_HOST, LLAMA_PORT);
                    cli.set_read_timeout(std::chrono::seconds(120));

                    auto result = cli.Post(
                        "/v1/chat/completions",
                        {{"Content-Type", "application/json"}},
                        payload.dump(), "application/json");

                    if (!result) {
                        send_event({{"type", "error"},
                                    {"message", "llama-server unreachable on 8080"}});
                        break;
                    }

                    json response;
                    try { response = json::parse(result->body); }
                    catch (...) {
                        send_event({{"type", "error"},
                                    {"message", "llama yanit parse hatasi"}});
                        break;
                    }

                    if (!response.contains("choices") || response["choices"].empty()) {
                        send_event({{"type", "error"},
                                    {"message", "Bos yanit"}});
                        break;
                    }

                    json msg = response["choices"][0]["message"];

                    // Tool calls var mi?
                    if (msg.contains("tool_calls") && !msg["tool_calls"].empty()) {
                        messages.push_back(msg);

                        for (const auto& tc : msg["tool_calls"]) {
                            std::string tname = tc["function"]["name"];
                            json targs = json::object();
                            try {
                                if (tc["function"]["arguments"].is_string()) {
                                    targs = json::parse(
                                        tc["function"]["arguments"].get<std::string>());
                                } else {
                                    targs = tc["function"]["arguments"];
                                }
                            } catch (...) { /* bos args */ }

                            send_event({
                                {"type", "tool_call"},
                                {"name", tname},
                                {"args", targs}
                            });

                            json tresult = dispatcher.execute(tname, targs);

                            send_event({
                                {"type", "tool_result"},
                                {"name", tname},
                                {"result", tresult}
                            });

                            messages.push_back({
                                {"role",         "tool"},
                                {"tool_call_id", tc.value("id", "")},
                                {"content",      tresult.dump()}
                            });
                        }
                        // dongu devam: yeni messages ile tekrar llama'ya soracagiz
                    } else {
                        // Tool yok, nihai cevap
                        std::string content = msg.value("content", "");
                        send_event({{"type", "content"}, {"text", content}});
                        break;
                    }
                }

                std::string done = "data: [DONE]\n\n";
                sink.write(done.data(), done.size());
                sink.done();
                return true;
            });
    });

    // ============================================
    // SERVER START
    // ============================================
    std::cout << "Dashboard: http://" << DASHBOARD_HOST << ":"
              << DASHBOARD_PORT << "\n";
    std::cout << "Tools: get_current, get_history_stats, set_sample_rate\n";
    std::cout.flush();

    bool ok = server.listen(DASHBOARD_HOST, DASHBOARD_PORT);
    if (!ok) {
        std::cerr << "HATA: server.listen() basarisiz. Port "
                  << DASHBOARD_PORT << " kullaniliyor olabilir.\n"
                  << "Kontrol: sudo lsof -i :" << DASHBOARD_PORT << "\n";
        sensors.stop();
        return 1;
    }

    sensors.stop();
    return 0;
}