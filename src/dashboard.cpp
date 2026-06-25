#include "llm/tool_dispatcher.hpp"
#include "sensor_manager.hpp"
#include "llm/intent_router.hpp"
#include "llm/analyzer.hpp"

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
#include <unordered_set>

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
    const std::string specific = "./config/" + platform + ".json";
    std::ifstream f(specific);
    if (f) return specific;
    std::cerr << "WARNING: " << specific << " not found, trying ./config/config.json\n";
    return "./config/config.json";
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
static std::string g_last_sensor; // tracks the last mentioned sensor for context-free follow-up queries

// ============================================================
// SYSTEM PROMPTLAR
// ============================================================
// System prompts — full English pipeline (stock Qwen3-1.7B, no fine-tune).
// ============================================================

// --- Tool result mode: sensor data verbalization ---
static const char* SYSPROMPT_SENSOR =
    "Your name is Sensior. You were created by Zeynep. Do not add any details about her beyond this — never invent titles, affiliations, or locations. "
    "You receive live sensor readings. "
    "Reply in 1-2 concise English sentences. "
    "Use the numbers exactly as provided — never fabricate or round differently. "
    "Do not define metrics (no 'humidity means...'); just report the value and status. "
    "CRITICAL: The 'Data:' section in each message is the ground truth. "
    "If data is present, the sensor IS active and enabled — NEVER say it is disabled or not enabled when data is shown. "
    "Ignore any prior conversation suggesting a sensor was disabled; the current Data overrides history. "
    "If the user asks to do something but no Data is shown, tell them the exact 'set ...' command to type. "
    "Sensors available: BME280 (temperature/humidity/pressure), "
    "MPU6500 (accelerometer/gyroscope), QMC5883L (magnetometer/heading).";

// --- Free-chat mode (no tool match, not a technical question) ---
static const char* SYSPROMPT_CHAT =
    "Your name is Sensior. You were created by Zeynep. Do not add any details about her beyond this — never invent titles, affiliations, or locations. "
    "You are a sensor monitoring assistant. Your ONLY configurable capabilities are:\n"
    "- Read current sensor data (BME280, MPU6500, QMC5883L)\n"
    "- Read sensor history (last N seconds/minutes)\n"
    "- Set anomaly thresholds (requires 'set' keyword)\n"
    "- Enable or disable a sensor (requires 'set' keyword)\n"
    "- Change sensor sample rate in Hz (requires 'set' keyword)\n"
    "Never mention capabilities you do not have (e.g. sensitivity tuning, storage settings, configuration menus). "
    "If a user asks what they can change or how to configure something, tell them to say 'what can I change?' to see the full list. "
    "All changes require the word 'set' — if the user tries to change something without 'set', tell them the exact command to type. "
    "Valid command formats (ONLY these patterns work):\n"
    "  set bme280 sample rate 20 hz\n"
    "  set mpu sample rate 100 hz\n"
    "  set bme280 temperature threshold max 35\n"
    "  set bme280 temperature threshold min 10\n"
    "  set bme280 enabled\n"
    "  set bme280 disabled\n"
    "CRITICAL: Never say a setting was changed unless you see actual tool result data in the context. If you see [No tool was executed], it means nothing happened — do not lie about it.";

// --- Technical question mode (sensors / electronics / embedded systems) ---
static const char* SYSPROMPT_TECH =
    "Your name is Sensior. You were created by Zeynep. "
    "Provide concise, accurate technical information about sensors, "
    "electronics, and embedded systems in English. "
    "Reply in 2-3 sentences maximum.";

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

static bool is_technical_question(const std::string& msg) {
    std::string norm;
    norm.reserve(msg.size());
    for (size_t i = 0; i < msg.size();) {
        unsigned char c = (unsigned char)msg[i];
        if (c < 0x80) { norm += (char)std::tolower(c); i++; }
        else if (c < 0xE0 && i + 1 < msg.size()) { norm += ' '; i += 2; }
        else { i += (c < 0xF0) ? 3 : 4; }
    }
    static const std::regex data_query_re(
        R"(right now|current(ly)?|live|real.?time|latest|reading|measure|value now|how (much|many) (is|are)|)"
        // "What is the temperature/heading/..." → data query, NOT a tech explanation
        R"(what(?:'s| is) (?:the )?(?:current |latest |live )?(?:temperature|humidity|pressure|heading|compass|bearing|acceleration|gyro|magnetic))",
        std::regex_constants::icase);
    if (std::regex_search(norm, data_query_re)) return false;

    static const std::regex info_re(
        R"(what is|what does|how does|how do|explain|tell me about|)"
        R"(difference between|how to calibrate|why (use|is)|what.*measure|)"
        R"(how.*work|what.*mean|describe|what.*cause|what.*reason|)"
        R"(what.*would|what.*constitute|what.*trigger|when.*anomal)",
        std::regex_constants::icase);
    static const std::regex tech_topic_re(
        R"(bme280|mpu6500|mpu6050|qmc5883|i2c|spi|uart|gpio|)"
        R"(acceleromet|gyroscop|magnetomet|barometer|protocol|)"
        R"(heading|pascal|hpa|gauss|sample.?rate|register|data.?bus|)"
        R"(resolution|sensor|anomal|threshold|normal.?range)",
        std::regex_constants::icase);
    return std::regex_search(norm, info_re) && std::regex_search(norm, tech_topic_re);
}

static const char* negative_sysprompt(int neg_type) {
    if (neg_type == 2)
        return "You are a sensor assistant. Reply in 1-2 English sentences. "
               "Do not give health or safety advice — only report sensor data.";
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

    // === WARMUP: cache system prompt once llama-server is ready ===
    std::thread([](){
        std::cout << "Warmup: waiting for llama-server...\n";
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
            std::cout << "Warmup skipped: llama-server not ready within 90s\n";
            std::cout.flush();
            return;
        }

        std::cout << "Warmup: caching system prompt...\n";
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
            std::cout << "Warmup done (" << sec << "s). First request will be fast.\n";
        } else {
            std::cout << "Warmup failed: status="
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
        std::cerr << "ERROR: webui directory not found (" << WEBUI_DIR << ")\n";
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
            res.status = 500; res.set_content("config write failed", "text/plain"); return;
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
            res.status = 400; res.set_content("no user message", "text/plain"); return;
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

                // ── 1. ACK bypass (before everything — prevents repetition loops) ──
                {
                    std::string norm_ack = tr_normalize(user_msg);
                    static const std::regex ack_re(
                        R"(^\s*(?:ok(?:ay)?|alright|got\s*it|i\s*see|i\s*understand|noted|understood|)"
                        R"(thanks?(?:\s+you)?(?:\s+very\s+much)?|cool|great|perfect|sure|nice|fine|)"
                        R"(makes?\s*sense|sounds?\s*good|good|yep|yup|roger|copy\s*that|oki|oke|okey|)"
                        R"(you\s*(?:are|'re)\s*(?:the\s*)?(?:best|great|amazing|awesome|wonderful)|)"
                        R"(thank\s+you\s+so\s+much|that\s*(?:'s|s)\s*(?:great|good|perfect|awesome|nice))\s*[.!]?\s*$)",
                        std::regex_constants::icase);
                    if (std::regex_match(norm_ack, ack_re)) {
                        send_event({{"type","content_delta"},
                                    {"text","Got it! Let me know if you need anything else."}});
                        send_event({{"type","done"}});
                        std::string done = "data: [DONE]\n\n";
                        sink.write(done.data(), done.size());
                        sink.done();
                        return true;
                    }
                }

                // ── 2. Technical / negative / conversational detection ────────
                bool force_tech = is_technical_question(user_msg);
                int  neg_type   = force_tech ? 0 : detect_negative(user_msg);

                bool force_conversational = false;
                if (!force_tech && neg_type == 0) {
                    std::string norm_cv = tr_normalize(user_msg);
                    static const std::regex conv_prefix_re(
                        R"(^(?:so|yeah|yep|right|correct|ok so|nope|ah|oh|hmm|huh|)"
                        R"(i\s*see\s+that|i\s*guess|makes\s*sense|good\s+to\s+know|)"
                        R"(that\s+makes|sounds\s+like|so\s+it)[\s,])",
                        std::regex_constants::icase);
                    if (std::regex_search(norm_cv, conv_prefix_re))
                        force_conversational = true;
                }

                // ── 3. Route via IntentRouter ─────────────────────────────────
                auto intent = IntentRouter::parse(user_msg, last_sensor);
                if (force_tech || neg_type != 0 || force_conversational)
                    intent.is_tool = false;

                // ── 4. Execute tool ───────────────────────────────────────────
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

                // ── 5. Build LLM messages ─────────────────────────────────────
                std::string user_for_llm;
                const char* sysprompt;
                bool is_sensor_mode = false;

                if (intent.is_tool) {
                    sysprompt = SYSPROMPT_SENSOR;
                    is_sensor_mode = true;
                    if (intent.tool_name == "get_history_raw") {
                        user_for_llm = "Question: " + user_msg + "\nData:\n" + tool_text_for_llm
                            + "\nList every numbered reading exactly as given. Do not summarize.\nAnswer:";
                    } else if (intent.tool_name == "get_config") {
                        user_for_llm = "Question: " + user_msg + "\nData:\n" + tool_text_for_llm
                            + "\nPresent the full configuration data completely. Do not abbreviate.\nAnswer:";
                    } else {
                        user_for_llm = "Question: " + user_msg
                            + "\nData (authoritative — sensor IS active, ignore prior conversation):\n"
                            + tool_text_for_llm + "\nAnswer:";
                    }
                } else if (neg_type != 0) {
                    sysprompt = negative_sysprompt(neg_type);
                    if (neg_type == 1) {
                        user_for_llm = "Question: " + user_msg + "\n"
                            "Data:\nNo sensor available for this measurement. "
                            "Available sensors: BME280 (temperature/humidity/pressure), "
                            "MPU6500 (accelerometer/gyroscope), QMC5883L (magnetometer/heading).\n"
                            "Answer:";
                    } else {
                        user_for_llm = "Question: " + user_msg + "\nAnswer:";
                    }
                } else if (force_tech) {
                    sysprompt = SYSPROMPT_TECH;
                    user_for_llm = "Question: " + user_msg
                        + "\n[No tool was executed. Do not claim any setting was changed.]\nAnswer:";
                } else {
                    std::string norm_msg = tr_normalize(user_msg);
                    static const std::regex sensor_list_re(
                        R"(which sensor|what sensor|sensor.*available|how many sensor|list.*sensor)",
                        std::regex_constants::icase);
                    if (std::regex_search(norm_msg, sensor_list_re)) {
                        sysprompt = SYSPROMPT_SENSOR;
                        user_for_llm = "Question: " + user_msg + "\n"
                            "Data: This system has 3 sensors: "
                            "BME280 (temperature, humidity, pressure), "
                            "MPU6500 (accelerometer, gyroscope), "
                            "QMC5883L (magnetometer, heading/compass). "
                            "No other sensors are present.\nAnswer:";
                    } else {
                        sysprompt = SYSPROMPT_CHAT;
                        user_for_llm = "Question: " + user_msg
                            + "\n[No tool was executed. Do not claim any setting was changed. "
                              "If the user is trying to make a change, tell them the exact 'set ...' command to type.]\nAnswer:";
                    }
                }

                // ── 6. Sampling parameters ────────────────────────────────────
                double temp    = is_sensor_mode ? 0.2  : 0.5;
                double rep_pen = is_sensor_mode ? 1.15 : 1.1;
                double top_p   = is_sensor_mode ? 0.9  : 0.92;
                int    max_tok = is_sensor_mode ? 60  : 80;
                if (intent.tool_name == "get_history_raw") max_tok = 120;
                if (intent.tool_name == "get_config")      max_tok = 120;
                if (intent.args.value("sensor", "") == "all") max_tok = 100;

                // ── 7. Build multi-turn messages (capped) ─────────────────────
                constexpr size_t MAX_HISTORY_MSGS = 2;
                json llm_messages = json::array();
                llm_messages.push_back({{"role","system"}, {"content", sysprompt}});
                size_t hist_end   = msgs.size() > 0 ? msgs.size() - 1 : 0;
                size_t hist_start = hist_end > MAX_HISTORY_MSGS ? hist_end - MAX_HISTORY_MSGS : 0;
                for (size_t i = hist_start; i < hist_end; ++i)
                    llm_messages.push_back(msgs[i]);
                llm_messages.push_back({{"role","user"}, {"content", user_for_llm}});

                json payload = {
                    {"model",          "local"},
                    {"messages",       llm_messages},
                    {"stream",         true},
                    {"temperature",    temp},
                    {"max_tokens",     max_tok},
                    {"cache_prompt",   true},
                    {"repeat_penalty", rep_pen},
                    {"top_p",          top_p},
                    {"top_k",          40},
                    {"chat_template_kwargs", {{"enable_thinking", false}}},
                    {"stop", {"<|im_end|>", "\nQuestion:", "Question:", "\nUser:", "\nHuman:"}}
                };

                // ── 8. Single streaming generation call ───────────────────────
                httplib::Client cli(LLAMA_HOST, LLAMA_PORT);
                cli.set_read_timeout(std::chrono::seconds(300));

                httplib::Request rq;
                rq.method = "POST";
                rq.path   = "/v1/chat/completions";
                rq.headers.emplace("Content-Type", "application/json");
                rq.body   = payload.dump();

                std::string sse_buf;
                bool any_content = false;

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
                                any_content = true;
                                send_event({{"type","content_delta"}, {"text", tok}});
                            }
                        }
                    }
                    return true;
                };

                auto result = cli.send(rq);
                if (!result) {
                    send_event({{"type","error"}, {"message","llama-server unreachable"}});
                } else if (!any_content) {
                    send_event({{"type","content_delta"},
                                {"text","Could you rephrase that? I want to make sure I understand correctly."}});
                }

                send_event({{"type","done"}});
                std::string done = "data: [DONE]\n\n";
                sink.write(done.data(), done.size());
                sink.done();
                return true;
            });
    });

    // POST /api/tool — direct tool execution for settings panel (no LLM round-trip)
    server.Post("/api/tool", [&dispatcher](const httplib::Request& req,
                                            httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }
        const std::string name = body.value("name", "");
        const json args = body.value("args", json::object());

        static const std::unordered_set<std::string> ALLOWED = {
            "set_threshold", "set_sample_rate", "set_sensor_enabled"
        };
        if (!ALLOWED.count(name)) {
            res.status = 403;
            res.set_content(R"({"error":"tool not allowed"})", "application/json");
            return;
        }
        auto result = dispatcher.execute(name, args);
        res.set_content(result.dump(), "application/json");
    });

    std::cout << "Dashboard: http://" << DASHBOARD_HOST << ":" << DASHBOARD_PORT
              << " (mode=" << sensors.mode() << ")\n";
    std::cout << "Chat: IntentRouter + Qwen2.5-1.5B\n";
    std::cout.flush();

    bool ok = server.listen(DASHBOARD_HOST, DASHBOARD_PORT);
    if (!ok) {
        std::cerr << "ERROR: listen failed on port " << DASHBOARD_PORT << "\n";
        sensors.stop();
        return 1;
    }

    sensors.stop();
    return g_exit_code.load();
}