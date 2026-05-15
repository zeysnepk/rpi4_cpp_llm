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

                // Bir tool call'in stream icinde parcali gelmesi durumu icin
                struct ToolCallAcc {
                    std::string id;
                    std::string name;
                    std::string arguments;
                };

                for (int iter = 0; iter < MAX_ITER; iter++) {
                    // Bu iterasyon icin state
                    std::string sse_buf;
                    std::vector<ToolCallAcc> tool_acc;
                    std::string finish_reason;

                    json payload = {
                        {"model",        "local"},
                        {"messages",     messages},
                        {"tools",        tools},
                        {"stream",       true},
                        {"temperature",  incoming.value("temperature", 0.3)},
                        {"max_tokens",   incoming.value("max_tokens", 512)},
                        {"cache_prompt", true}
                    };

                    httplib::Client cli(LLAMA_HOST, LLAMA_PORT);
                    cli.set_read_timeout(std::chrono::seconds(300));

                    httplib::Request rq;
                    rq.method = "POST";
                    rq.path   = "/v1/chat/completions";
                    rq.headers.emplace("Content-Type", "application/json");
                    rq.headers.emplace("Accept",       "text/event-stream");
                    rq.body   = payload.dump();

                    // llama-server'dan gelen SSE chunk'lari parse et,
                    // content varsa direkt browser'a forward et,
                    // tool_calls varsa biriktir
                    rq.content_receiver = [&](const char* data, size_t len,
                                              uint64_t, uint64_t) -> bool {
                        sse_buf.append(data, len);

                        size_t pos;
                        while ((pos = sse_buf.find('\n')) != std::string::npos) {
                            std::string line = sse_buf.substr(0, pos);
                            sse_buf.erase(0, pos + 1);
                            if (!line.empty() && line.back() == '\r') line.pop_back();

                            if (line.rfind("data: ", 0) != 0) continue;
                            std::string data_str = line.substr(6);
                            if (data_str == "[DONE]") continue;

                            json chunk;
                            try { chunk = json::parse(data_str); } catch (...) { continue; }

                            if (!chunk.contains("choices") || chunk["choices"].empty()) continue;
                            const auto& choice = chunk["choices"][0];

                            if (choice.contains("finish_reason") &&
                                !choice["finish_reason"].is_null()) {
                                finish_reason = choice["finish_reason"].get<std::string>();
                            }

                            if (!choice.contains("delta")) continue;
                            const auto& delta = choice["delta"];

                            // Content -> browser'a forward
                            if (delta.contains("content") && delta["content"].is_string()) {
                                std::string text = delta["content"].get<std::string>();
                                if (!text.empty()) {
                                    json ev = {{"type","content_delta"}, {"text", text}};
                                    std::string s = "data: " + ev.dump() + "\n\n";
                                    if (!sink.write(s.data(), s.size())) return false;
                                }
                            }

                            // Tool call deltalarini birikir
                            if (delta.contains("tool_calls") &&
                                delta["tool_calls"].is_array()) {
                                for (const auto& tc : delta["tool_calls"]) {
                                    int idx = tc.value("index", 0);
                                    while ((int)tool_acc.size() <= idx) tool_acc.push_back({});
                                    auto& acc = tool_acc[idx];

                                    if (tc.contains("id") && tc["id"].is_string())
                                        acc.id = tc["id"].get<std::string>();

                                    if (tc.contains("function")) {
                                        const auto& fn = tc["function"];
                                        if (fn.contains("name") && fn["name"].is_string())
                                            acc.name = fn["name"].get<std::string>();
                                        if (fn.contains("arguments") && fn["arguments"].is_string())
                                            acc.arguments += fn["arguments"].get<std::string>();
                                    }
                                }
                            }
                        }
                        return true;
                    };

                    auto result = cli.send(rq);
                    if (!result) {
                        send_event({{"type","error"},
                                    {"message","llama-server unreachable"}});
                        break;
                    }

                    // Stream bitti. Tool call var mi?
                    if (finish_reason == "tool_calls" && !tool_acc.empty()) {
                        // Assistant mesajini ekle (tool_calls icerikli)
                        json assistant_msg = {
                            {"role", "assistant"},
                            {"content", nullptr},
                            {"tool_calls", json::array()}
                        };
                        for (const auto& acc : tool_acc) {
                            assistant_msg["tool_calls"].push_back({
                                {"id", acc.id},
                                {"type", "function"},
                                {"function", {
                                    {"name", acc.name},
                                    {"arguments", acc.arguments}
                                }}
                            });
                        }
                        messages.push_back(assistant_msg);

                        // Her tool'u calistir, sonuc event'lerini yolla,
                        // messages'a tool sonucunu ekle
                        for (const auto& acc : tool_acc) {
                            json targs = json::object();
                            try { targs = json::parse(acc.arguments); } catch (...) {}

                            send_event({{"type","tool_call"},
                                        {"name", acc.name},
                                        {"args", targs}});

                            json tresult = dispatcher.execute(acc.name, targs);

                            send_event({{"type","tool_result"},
                                        {"name", acc.name},
                                        {"result", tresult}});

                            messages.push_back({
                                {"role",         "tool"},
                                {"tool_call_id", acc.id},
                                {"content",      tresult.dump()}
                            });
                        }
                        // Dongu devam: yeni messages ile tekrar stream et
                    } else {
                        // Tool yok, content zaten stream edildi, bitir
                        break;
                    }
                }

                send_event({{"type","done"}});
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