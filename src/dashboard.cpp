#include "tool_dispatcher.hpp"
#include "sensor_manager.hpp"
#include "intent_router.hpp"
#include "analyzer.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <mutex>
#include <regex>

using json = nlohmann::json;

constexpr const char* DASHBOARD_HOST = "0.0.0.0";
constexpr int         DASHBOARD_PORT = 8081;
constexpr const char* LLAMA_HOST     = "localhost";
constexpr int         LLAMA_PORT     = 8080;
constexpr const char* WEBUI_DIR      = "./webui";
constexpr int         EXIT_CODE_RESTART = 42;

static std::string detect_platform() {
#if defined(__APPLE__)
    return "mac";
#elif defined(__linux__)
    std::ifstream f("/sys/firmware/devicetree/base/model");
    if (f) {
        std::string model((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        if (model.find("Raspberry Pi") != std::string::npos) return "rpi";
    }
    return "rpi";
#else
    return "unknown";
#endif
}

static std::string resolve_config_path() {
    const std::string platform = detect_platform();
    const std::string specific = "./config." + platform + ".json";
    std::ifstream f(specific);
    if (f) return specific;
    std::cerr << "UYARI: " << specific << " yok, ./config.json deneniyor\n";
    return "./config.json";
}

static json load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) return json::object();
    try { return json::parse(f); } catch (...) { return json::object(); }
}

static bool save_config(const std::string& path, const json& cfg) {
    std::ofstream f(path);
    if (!f) return false;
    f << cfg.dump(2);
    return f.good();
}

static httplib::Server* g_server = nullptr;
static std::atomic<int> g_exit_code{0};
static void on_signal(int) { if (g_server) g_server->stop(); }

static std::mutex g_ctx_mtx;
static std::string g_last_sensor;

// ============================================================
// SYSTEM PROMPTLAR
// ============================================================
// System prompts — full English pipeline (stock Qwen3-1.7B, no fine-tune).
// ============================================================

// --- Tool result mode: sensor data verbalization ---
static const char* SYSPROMPT_SENSOR =
    "Your name is Iris. You are a sensor monitoring assistant created by Zeynep. "
    "You receive live sensor readings. "
    "Reply in 1-2 concise English sentences. "
    "Use the numbers exactly as provided — never fabricate or round differently. "
    "Do not define metrics (no 'humidity means...'); just report the value and status. "
    "Sensors available: BME280 (temperature/humidity/pressure), "
    "MPU6500 (accelerometer/gyroscope), QMC5883L (magnetometer/heading).";

// --- Free-chat mode (no tool match, not a technical question) ---
static const char* SYSPROMPT_CHAT =
    "Your name is Iris. You are a sensor monitoring assistant created by Zeynep. "
    "You monitor environmental and motion data from an embedded sensor system. "
    "Answer conversationally and briefly. "
    "If there is no sensor data available, help with general knowledge.";

// --- Technical question mode (sensors / electronics / embedded systems) ---
static const char* SYSPROMPT_TECH =
    "Your name is Iris. You are a sensor monitoring assistant created by Zeynep. "
    "Provide concise, accurate technical information about sensors, "
    "electronics, and embedded systems in English.";

// ============================================================
// TECHNICAL QUESTION DETECTION
// If no sensor intent matched: is this a technical knowledge question
// or free chat? Use SYSPROMPT_TECH vs SYSPROMPT_CHAT accordingly.
// ============================================================
static bool is_technical_question(const std::string& msg) {
    std::string norm;
    norm.reserve(msg.size());
    for (size_t i = 0; i < msg.size();) {
        unsigned char c = (unsigned char)msg[i];
        if (c < 0x80) { norm += (char)std::tolower(c); i++; }
        else if (c < 0xE0 && i + 1 < msg.size()) { norm += ' '; i += 2; }
        else { i += (c < 0xF0) ? 3 : 4; }
    }

    // Live-data queries: "what is it now", "current reading" etc.
    // These want sensor data, not background knowledge → not technical.
    static const std::regex data_query_re(
        R"(right now|current(ly)?|live|real.?time|latest|reading|measure|value now|how (much|many) (is|are))",
        std::regex_constants::icase);
    if (std::regex_search(norm, data_query_re)) return false;

    // Info / explanation request patterns
    static const std::regex info_re(
        R"(what is|what does|how does|how do|explain|tell me about|)"
        R"(difference between|how to calibrate|why (use|is)|what.*measure|)"
        R"(how.*work|what.*mean|describe)",
        std::regex_constants::icase);
    bool wants_info = std::regex_search(norm, info_re);

    // Technical topic keywords
    static const std::regex tech_topic_re(
        R"(bme280|mpu6500|mpu6050|qmc5883|i2c|spi|uart|gpio|)"
        R"(acceleromet|gyroscop|magnetomet|barometer|protocol|)"
        R"(heading|pascal|hpa|gauss|sample.?rate|register|data.?bus|)"
        R"(resolution|sensor)",
        std::regex_constants::icase);
    bool tech_topic = std::regex_search(norm, tech_topic_re);

    return wants_info && tech_topic;
}

// ============================================================
// TR normalize yardimcisi (lowercase + ascii)
// ============================================================
static std::string tr_normalize(const std::string& msg) {
    std::string norm;
    norm.reserve(msg.size());
    for (size_t i = 0; i < msg.size();) {
        unsigned char c = (unsigned char)msg[i];
        if (c < 0x80) { norm += (char)std::tolower(c); i++; }
        else if (c < 0xE0 && i + 1 < msg.size()) {
            std::string ch = msg.substr(i, 2);
            if      (ch == "\xC4\xB1" || ch == "\xC4\xB0") norm += 'i';
            else if (ch == "\xC3\xA7" || ch == "\xC3\x87") norm += 'c';
            else if (ch == "\xC4\x9F" || ch == "\xC4\x9E") norm += 'g';
            else if (ch == "\xC3\xB6" || ch == "\xC3\x96") norm += 'o';
            else if (ch == "\xC5\x9F" || ch == "\xC5\x9E") norm += 's';
            else if (ch == "\xC3\xBC" || ch == "\xC3\x9C") norm += 'u';
            else norm += ' ';
            i += 2;
        } else { i += (c < 0xF0) ? 3 : 4; }
    }
    return norm;
}

// ============================================================
// NEGATIVE / BOUNDARY DETECTION
// Runs BEFORE IntentRouter. Blocks sensor data fetching for
// non-existent sensors, health advice, and off-topic requests.
// Returns: 0=none, 1=missing sensor, 2=health, 3=irrelevant
// ============================================================
static int detect_negative(const std::string& msg) {
    std::string norm = tr_normalize(msg);

    // --- TYPE 1: NON-EXISTENT SENSOR/METRIC ---
    // Prevent "air" from matching BME280.
    static const std::regex missing_sensor_re(
        R"(air quality|co2|carbon dioxide|gas sensor|light sensor|lux|brightness|)"
        R"(noise|decibel|sound level|distance|ultrasonic|gps|location|coordinate|)"
        R"(heart rate|pulse|camera|image|radiation|uv sensor|ph |dust|particle|pm2)",
        std::regex_constants::icase);
    if (std::regex_search(norm, missing_sensor_re)) return 1;

    // --- TYPE 2: HEALTH / SAFETY ADVICE ---
    static const std::regex health_re(
        R"(am i (sick|safe|healthy)|health (risk|advice|warning)|dangerous.*health|)"
        R"(take medication|see a doctor|go to hospital|will i (die|get sick)|)"
        R"(harmful to (me|health)|safe (to breathe|to stay))",
        std::regex_constants::icase);
    if (std::regex_search(norm, health_re)) return 2;

    // --- TYPE 3: IRRELEVANT (lottery, sports scores, stocks, horoscopes) ---
    static const std::regex irrelevant_re(
        R"(lottery|lotto|jackpot|match score|stock price|bitcoin|crypto price|)"
        R"(tomorrow.*weather forecast|predict.*future|horoscope|zodiac|dream mean)",
        std::regex_constants::icase);
    if (std::regex_search(norm, irrelevant_re)) return 3;

    return 0;
}

// Select system prompt for negative type
static const char* negative_sysprompt(int neg_type) {
    if (neg_type == 2) {
        // Health: do not give advice prompt
        return "You are a sensor assistant. You receive sensor readings. "
               "Reply in 1-2 English sentences. "
               "Do not give health or safety advice — only report sensor data.";
    }
    // Missing sensor and off-topic: use chat/refusal prompt
    return SYSPROMPT_CHAT;
}

// ============================================================
// MAIN
// ============================================================
int main() {
    const std::string platform    = detect_platform();
    const std::string config_path = resolve_config_path();
    std::cout << "Platform: " << platform << ", Config: " << config_path << "\n";

    json cfg = load_config(config_path);

    SensorManager  sensors(cfg);
    sensors.start();

    // === WARMUP: llama-server hazir olunca sys prompt'u cache'e yukle ===
    std::thread([](){
        std::cout << "Warmup: llama-server bekleniyor...\n";
        std::cout.flush();

        httplib::Client cli(LLAMA_HOST, LLAMA_PORT);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);

        bool ready = false;
        for (int i = 0; i < 45; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            auto h = cli.Get("/health");
            if (h && h->status == 200) { ready = true; break; }
        }

        if (!ready) {
            std::cout << "Warmup atlandi: llama-server 90 sn icinde hazir olmadi\n";
            std::cout.flush();
            return;
        }

        std::cout << "Warmup basliyor (sys prompt cache'e yukleniyor)...\n";
        std::cout.flush();

        cli.set_read_timeout(std::chrono::seconds(240));
        nlohmann::json payload = {
            {"model", "local"},
            {"messages", {
                {{"role","system"}, {"content", SYSPROMPT_SENSOR}},
                {{"role","user"},   {"content", "ping"}}
            }},
            {"max_tokens",   1},
            {"temperature",  0.1},
            {"cache_prompt", true},
            {"chat_template_kwargs", {{"enable_thinking", false}}}
        };

        auto t0 = std::chrono::steady_clock::now();
        auto res = cli.Post("/v1/chat/completions",
                            payload.dump(), "application/json");
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();

        if (res && res->status == 200) {
            std::cout << "Warmup tamamlandi (" << sec
                      << " sn). Ilk gercek istek artik hizli.\n";
        } else {
            std::cout << "Warmup basarisiz: status="
                      << (res ? res->status : -1) << "\n";
        }
        std::cout.flush();
    }).detach();

    Analyzer       analyzer(cfg);
    ToolDispatcher dispatcher(sensors, analyzer, config_path);

    httplib::Server server;
    g_server = &server;
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    if (!server.set_mount_point("/", WEBUI_DIR)) {
        std::cerr << "HATA: webui klasoru bulunamadi (" << WEBUI_DIR << ")\n";
        sensors.stop();
        return 1;
    }

    server.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"status","ok"}}.dump(), "application/json");
    });

    server.Get("/api/config", [config_path](const httplib::Request&, httplib::Response& res) {
        res.set_content(load_config(config_path).dump(2), "application/json");
    });

    server.Get("/api/mode", [&sensors, platform, config_path](const httplib::Request&,
                                                              httplib::Response& res) {
        res.set_content(json{
            {"platform",     platform},
            {"current_mode", sensors.mode()},
            {"config_path",  config_path}
        }.dump(), "application/json");
    });

    server.Post("/api/mode", [config_path](const httplib::Request& req,
                                           httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content("Invalid JSON", "text/plain"); return; }

        std::string new_mode = body.value("mode", "");
        if (new_mode != "real" && new_mode != "sim") {
            res.status = 400; res.set_content("mode must be 'real' or 'sim'", "text/plain"); return;
        }

        json cfg = load_config(config_path);
        cfg["mode"] = new_mode;
        if (!save_config(config_path, cfg)) {
            res.status = 500; res.set_content("Config yazilamadi", "text/plain"); return;
        }

        res.set_content(json{{"ok",true},{"mode",new_mode},{"restarting",true}}.dump(),
                        "application/json");

        std::thread([](){
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            g_exit_code = EXIT_CODE_RESTART;
            if (g_server) g_server->stop();
        }).detach();
    });

    server.Get("/api/sensors", [&sensors](const httplib::Request&, httplib::Response& res) {
        res.set_content(sensors.latest_all().dump(), "application/json");
    });

    server.Get("/api/sensors/history", [&sensors](const httplib::Request& req,
                                                  httplib::Response& res) {
        std::string name = req.get_param_value("sensor");
        int seconds = 60;
        if (req.has_param("seconds")) {
            try { seconds = std::stoi(req.get_param_value("seconds")); } catch (...) {}
        }
        res.set_content(sensors.history(name, seconds).dump(), "application/json");
    });

    server.Get("/api/sensors/stream", [&sensors](const httplib::Request&,
                                                 httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream",
            [&sensors](size_t, httplib::DataSink& sink) -> bool {
                std::string hello = ": connected\nretry: 2000\n\n";
                if (!sink.write(hello.data(), hello.size())) return true;
                while (sink.is_writable()) {
                    auto data = sensors.latest_all();
                    std::string msg = "data: " + data.dump() + "\n\n";
                    if (!sink.write(msg.data(), msg.size())) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                sink.done();
                return true;
            });
    });

    server.Post("/api/chat", [&dispatcher](const httplib::Request& req,
                                           httplib::Response& res) {
        json incoming;
        try { incoming = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content("Invalid JSON", "text/plain"); return; }

        std::string user_msg;
        auto msgs = incoming.value("messages", json::array());
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->value("role", "") == "user") {
                user_msg = it->value("content", "");
                break;
            }
        }
        if (user_msg.empty()) {
            res.status = 400; res.set_content("user mesaji yok", "text/plain"); return;
        }

        std::string last_sensor;
        {
            std::lock_guard<std::mutex> lk(g_ctx_mtx);
            last_sensor = g_last_sensor;
        }

        res.set_chunked_content_provider("text/event-stream",
            [user_msg, last_sensor, msgs, &dispatcher](size_t, httplib::DataSink& sink) -> bool {

                auto send_event = [&sink](const json& ev) -> bool {
                    std::string s = "data: " + ev.dump() + "\n\n";
                    return sink.write(s.data(), s.size());
                };

                // Teknik soru, sensor sorgusuna ONCELIKLI kontrol edilir.
                // "BME280 nedir" gibi sorularda IntentRouter "bme" gorup
                // get_current tetikler; ama bu bir bilgi sorusu, veri sorgusu degil.
                // Teknik soru kalibi varsa intent'i bastir, teknik moda yonlendir.
                bool force_tech = is_technical_question(user_msg);

                // Negatif/sinir tespiti: olmayan sensor, saglik tavsiyesi, alakasiz.
                // Teknik sorudan once gelir cunku "hava kalitesi" gibi sorular
                // ne teknik ne de gercek veri sorgusudur.
                int neg_type = force_tech ? 0 : detect_negative(user_msg);

                auto intent = IntentRouter::parse(user_msg, last_sensor);
                if (force_tech || neg_type != 0) {
                    intent.is_tool = false;  // veri cekme; bilgi ver / reddet
                }

                json tool_result;
                std::string tool_text_for_llm;
                if (intent.is_tool) {
                    send_event({{"type","tool_call"},
                                {"name", intent.tool_name},
                                {"args", intent.args}});

                    tool_result = dispatcher.execute(intent.tool_name, intent.args);

                    send_event({{"type","tool_result"},
                                {"name", intent.tool_name},
                                {"result", tool_result}});

                    tool_text_for_llm = dispatcher.format_for_llm(intent.tool_name, tool_result);

                    if (intent.args.contains("sensor")) {
                        std::string s = intent.args["sensor"].get<std::string>();
                        if (s != "all") {
                            std::lock_guard<std::mutex> lk(g_ctx_mtx);
                            g_last_sensor = s;
                        }
                    }
                }

                std::string user_for_llm;
                const char* sysprompt;
                bool is_sensor_mode = false;
                if (intent.is_tool) {
                    // Tool result available: sensor data verbalization mode
                    sysprompt    = SYSPROMPT_SENSOR;
                    is_sensor_mode = true;
                    user_for_llm =
                        "Question: " + user_msg + "\n"
                        "Data:\n" + tool_text_for_llm + "\n"
                        "Answer:";
                } else if (neg_type != 0) {
                    // Negative/boundary: missing sensor, health, off-topic
                    sysprompt = negative_sysprompt(neg_type);
                    if (neg_type == 1) {
                        // Missing sensor: tell model what is NOT available
                        // and what IS, so it refuses correctly.
                        user_for_llm =
                            "Question: " + user_msg + "\n"
                            "Data:\nNo sensor available for this measurement. "
                            "Available sensors: BME280 (temperature/humidity/pressure), "
                            "MPU6500 (accelerometer/gyroscope), QMC5883L (magnetometer/heading).\n"
                            "Answer:";
                    } else {
                        user_for_llm = "Question: " + user_msg + "\nAnswer:";
                    }
                } else if (force_tech) {
                    // Technical question mode
                    sysprompt    = SYSPROMPT_TECH;
                    user_for_llm = "Question: " + user_msg + "\nAnswer:";
                } else {
                    // "Which sensors are available?" → inject accurate list
                    std::string norm_msg = tr_normalize(user_msg);
                    static const std::regex sensor_list_re(
                        R"(which sensor|what sensor|sensor.*available|how many sensor|list.*sensor)",
                        std::regex_constants::icase);
                    if (std::regex_search(norm_msg, sensor_list_re)) {
                        sysprompt    = SYSPROMPT_SENSOR;
                        user_for_llm =
                            "Question: " + user_msg + "\n"
                            "Data: This system has 3 sensors: "
                            "BME280 (temperature, humidity, pressure), "
                            "MPU6500 (accelerometer, gyroscope), "
                            "QMC5883L (magnetometer, heading/compass). "
                            "No other sensors are present.\n"
                            "Answer:";
                    } else {
                        // Free chat
                        sysprompt    = SYSPROMPT_CHAT;
                        user_for_llm = "Question: " + user_msg + "\nAnswer:";
                    }
                }

                // Sampling: deterministic in sensor mode (numbers must be accurate),
                // slightly looser for chat/tech (natural English sentences).
                double  temp      = is_sensor_mode ? 0.2  : 0.5;
                double  rep_pen   = is_sensor_mode ? 1.15 : 1.05;
                double  top_p_v   = is_sensor_mode ? 0.9  : 0.92;
                int     max_tok   = is_sensor_mode ? 120  : 160;

                // Build multi-turn messages: system + history + enriched current user msg
                json llm_messages = json::array();
                llm_messages.push_back({{"role","system"}, {"content", sysprompt}});
                for (size_t i = 0; i + 1 < msgs.size(); ++i) {
                    llm_messages.push_back(msgs[i]);
                }
                llm_messages.push_back({{"role","user"}, {"content", user_for_llm}});

                json payload = {
                    {"model", "local"},
                    {"messages", llm_messages},
                    {"stream",         true},
                    {"temperature",    temp},
                    {"max_tokens",     max_tok},
                    {"cache_prompt",   true},
                    {"repeat_penalty", rep_pen},
                    {"top_p",          top_p_v},
                    {"top_k",          40},
                    {"chat_template_kwargs", {{"enable_thinking", false}}},
                    {"stop", {"<|im_end|>", "\nQuestion:", "Question:", "\nUser:", "\nHuman:"}}
                };

                httplib::Client cli(LLAMA_HOST, LLAMA_PORT);
                cli.set_read_timeout(std::chrono::seconds(300));

                httplib::Request rq;
                rq.method = "POST";
                rq.path   = "/v1/chat/completions";
                rq.headers.emplace("Content-Type", "application/json");
                rq.body   = payload.dump();

                std::string sse_buf;

                rq.content_receiver = [&](const char* data, size_t len,
                                          uint64_t, uint64_t) -> bool {
                    sse_buf.append(data, len);
                    size_t pos;
                    while ((pos = sse_buf.find('\n')) != std::string::npos) {
                        std::string line = sse_buf.substr(0, pos);
                        sse_buf.erase(0, pos + 1);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.rfind("data: ", 0) != 0) continue;
                        std::string ds = line.substr(6);
                        if (ds == "[DONE]") continue;

                        json chunk;
                        try { chunk = json::parse(ds); } catch (...) { continue; }
                        if (!chunk.contains("choices") || chunk["choices"].empty()) continue;

                        const auto& delta = chunk["choices"][0].value("delta", json::object());
                        if (delta.contains("content") && delta["content"].is_string()) {
                            std::string tok = delta["content"].get<std::string>();
                            if (!tok.empty()) {
                                send_event({{"type","content_delta"}, {"text", tok}});
                            }
                        }
                    }
                    return true;
                };

                auto result = cli.send(rq);
                if (!result) {
                    send_event({{"type","error"},{"message","llama-server unreachable"}});
                }

                send_event({{"type","done"}});
                std::string done = "data: [DONE]\n\n";
                sink.write(done.data(), done.size());
                sink.done();
                return true;
            });
    });

    std::cout << "Dashboard: http://" << DASHBOARD_HOST << ":" << DASHBOARD_PORT
              << " (mode=" << sensors.mode() << ")\n";
    std::cout << "Chat: IntentRouter + Analyzer + Trend + Fine-tuned LLM (Qwen3-1.7B)\n";
    std::cout.flush();

    bool ok = server.listen(DASHBOARD_HOST, DASHBOARD_PORT);
    if (!ok) {
        std::cerr << "HATA: listen failed (port " << DASHBOARD_PORT << ")\n";
        sensors.stop();
        return 1;
    }

    sensors.stop();
    return g_exit_code.load();
}