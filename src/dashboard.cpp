#include "translator.hpp"
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
// SYSPROMPT (10 few-shot ornegi - kalite triadi icin optimize)
// ============================================================
static const char* SYSPROMPT_INTERPRET_TR =
    "Sen sensor asistanisin. Kullaniciya sensor verileri hakkinda KISA, NET, "
    "TURKCE cevap verirsin. Sayilar zaten yuvarlanmis halde gelir, oldugu gibi kullan.\n"
    "\n"
    "KESIN KURALLAR:\n"
    "- Verideki sensor adini ve degerleri AYNEN kullan, kendinden uydurma.\n"
    "- Durum bilgisi geldiyse (Normal/ANOMALI) ona uy, kendi kararini katma.\n"
    "- Cevap 1-3 cumle. Uzun aciklama yapma.\n"
    "- Asagidaki ornekleri taklit et:\n"
    "\n"
    "---Ornek 1---\n"
    "Soru: Sicaklik ne?\n"
    "Veri:\n"
    "Sensor verisi:\n"
    "- sicaklik: 26.50 °C [normal]\n"
    "- nem: 54.20 %\n"
    "- basinc: 1013.40 hPa\n"
    "Cevap: BME280 su an 26.5°C, nem %54.2, basinc 1013.4 hPa. Oda sicakligi normal.\n"
    "\n"
    "---Ornek 2---\n"
    "Soru: Sicakligi yorumlar misin?\n"
    "Veri:\n"
    "Sensor verisi:\n"
    "- sicaklik: 26.50 °C [normal]\n"
    "Cevap: 26.5°C, oda sicakligi icin normal aralikta. Konfor kosullari iyi.\n"
    "\n"
    "---Ornek 3---\n"
    "Soru: BME280'i 5 Hz yap\n"
    "Veri: Tool sonucu: bme280 sensoru 5 Hz olarak ayarlandi. Config'e kaydedildi.\n"
    "Cevap: BME280 ornekleme hizi 5 Hz olarak ayarlandi.\n"
    "\n"
    "---Ornek 4---\n"
    "Soru: Son 30 sn Z ivmesi\n"
    "Veri:\n"
    "Son 30 saniye - Z ivmesi:\n"
    "- Ortalama: 1.00 g\n"
    "- Min: 0.93 g\n"
    "- Max: 1.07 g\n"
    "- Son okuma: 1.00 g\n"
    "- Olcum sayisi: 1304\n"
    "- Trend: sabit (toplam degisim 0.00 g)\n"
    "\n>>> SONUC: Normal (statik gravity araliginda) <<<\n"
    "Cevap: Son 30 saniyede Z ivmesi 0.93-1.07 g arasinda, ortalama 1.00 g. Durum statik ve normal.\n"
    "\n"
    "---Ornek 5---\n"
    "Soru: Anomali var mi yorumla\n"
    "Veri:\n"
    "Son 60 saniye - Z ivmesi:\n"
    "- Ortalama: 1.05 g\n"
    "- Min: 0.40 g\n"
    "- Max: 2.10 g\n"
    "- Olcum sayisi: 1200\n"
    "- Trend: artiyor (toplam degisim 0.50 g, %50.0)\n"
    "\n>>> SONUC: ANOMALI VAR (high) - max 2.10 > esik 1.08 <<<\n"
    "Cevap: Anomali tespit edildi (siddet: yuksek). Z ivmesi 2.10 g'ye kadar cikmis, statik durum icin beklenmedik. Cihaz hareket etmis veya darbe olabilir.\n"
    "\n"
    "---Ornek 6---\n"
    "Soru: Sicaklik trend\n"
    "Veri:\n"
    "Son 60 saniye - sicaklik:\n"
    "- Ortalama: 25.80 °C\n"
    "- Min: 25.20 °C\n"
    "- Max: 26.30 °C\n"
    "- Trend: artiyor (toplam degisim 1.10 °C, %4.4)\n"
    "\n>>> SONUC: Normal (oda sicakligi araliginda) <<<\n"
    "Cevap: Son 60 saniyede sicaklik 1.1°C arttiginda, 25.2'den 26.3°C'ye yukseldi. Hala normal aralikta.\n"
    "\n"
    "---Ornek 7---\n"
    "Soru: Sorun var mi\n"
    "Veri:\n"
    "Son 60 saniye - nem:\n"
    "- Ortalama: 75.40 %\n"
    "- Max: 82.10 %\n"
    "- Trend: artiyor (toplam degisim 8.20 %)\n"
    "\n>>> SONUC: ANOMALI (medium) - max 82.10 > esik 70.00 <<<\n"
    "Cevap: Nem cok yuksek (max %82.1, esik %70). Ortam nemli, havalandirma onerilir.\n"
    "\n"
    "---Ornek 8---\n"
    "Soru: heading nedir\n"
    "Veri:\n"
    "Sensor verisi:\n"
    "- yon (heading): 87.30 °\n"
    "- X manyetik: 0.30 G\n"
    "- Y manyetik: -0.10 G\n"
    "- Z manyetik: -0.32 G\n"
    "Cevap: Su an heading 87.3° (yaklasik Dogu yonu). Manyetik alan X=0.30 G, Y=-0.10 G, Z=-0.32 G.\n"
    "\n"
    "---Ornek 9---\n"
    "Soru: Hava nasil\n"
    "Veri:\n"
    "Sensor verisi:\n"
    "- sicaklik: 24.80 °C [normal]\n"
    "- nem: 48.00 % [normal]\n"
    "- basinc: 1014.20 hPa\n"
    "Cevap: Hava iyi durumda. 24.8°C, nem %48, basinc 1014.2 hPa. Konfor kosullari ideal.\n"
    "\n"
    "---Ornek 10---\n"
    "Soru: Tum sensorler\n"
    "Veri:\n"
    "Tum sensor verileri:\n"
    "bme280:\n"
    "  sicaklik: 25.00 °C\n"
    "  nem: 50.00 %\n"
    "mpu6050:\n"
    "  Z ivmesi: 1.00 g\n"
    "qmc5883l:\n"
    "  yon (heading): 90.00 °\n"
    "Cevap: BME280: 25.0°C, nem %50. MPU6500: Z ivme 1.00 g (statik). QMC5883L: heading 90° (Dogu).\n"
    "\n"
    "---Ornek 11---\n"
    "Soru: Anomali var mi yorumla\n"
    "Veri:\n"
    "Son 60 saniye - Z ivmesi:\n"
    "- Ortalama: 1.00 g\n"
    "- Min: 0.95 g\n"
    "- Max: 1.05 g\n"
    "- Trend: sabit (toplam degisim 0.00 g)\n"
    "\n>>> SONUC: Normal (statik gravity araliginda) <<<\n"
    "Cevap: Hayir, anomali yok. Z ivmesi 0.95-1.05 g arasinda, statik durum icin normal.\n"
    "\n"
    "---Ornek 12---\n"
    "Soru: Sorun var mi?\n"
    "Veri:\n"
    "Son 60 saniye - sicaklik:\n"
    "- Ortalama: 25.50 °C\n"
    "- Min: 24.80 °C\n"
    "- Max: 26.20 °C\n"
    "- Trend: sabit\n"
    "\n>>> SONUC: Normal (oda sicakligi araliginda) <<<\n"
    "Cevap: Sorun yok. Sicaklik 25.5°C civarinda, oda sicakligi normal aralikta.\n"
    "\n"
    "SIMDI cevap yaz. Sadece son cevabi yaz, ornekleri tekrar yazma. Veri ile uyumsuz bilgi katma.";

static const char* SYSPROMPT_CHAT_TR =
    "Sen sensor asistanisin. Sensorler: BME280 (sicaklik/nem/basinc), "
    "MPU6500 (ivme/gyro), QMC5883L (manyetik/heading). "
    "Selamlama veya genel sorulara KISA, dogal Turkce cevap ver.";

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

    std::thread([](){
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cout << "Warmup basliyor (sys prompt cache'e yukleniyor)...\n";
        std::cout.flush();

        httplib::Client cli(LLAMA_HOST, LLAMA_PORT);
        cli.set_read_timeout(std::chrono::seconds(120));

        nlohmann::json payload = {
            {"model", "local"},
            {"messages", {
                {{"role","system"}, {"content", SYSPROMPT_INTERPRET_TR}},
                {{"role","user"},   {"content", "ping"}}
            }},
            {"max_tokens",   1},
            {"temperature",  0.1},
            {"cache_prompt", true}
        };

        auto t0 = std::chrono::steady_clock::now();
        auto res = cli.Post("/v1/chat/completions",
                            payload.dump(), "application/json");
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();

        if (res && res->status == 200) {
            std::cout << "Warmup tamamlandi (" << sec
                      << " sn). Ilk gercek istek artik hizli olacak.\n";
        } else {
            std::cout << "Warmup basarisiz: status="
                      << (res ? res->status : -1) << "\n";
        }
        std::cout.flush();
    }).detach();

    Translator     translator(cfg);
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

    server.Get("/api/translator", [&translator](const httplib::Request&,
                                                httplib::Response& res) {
        if (translator.enabled() && !translator.available()) translator.refresh_status();
        res.set_content(json{
            {"enabled",   translator.enabled()},
            {"available", translator.available()}
        }.dump(), "application/json");
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
            [user_msg, last_sensor, &dispatcher](size_t, httplib::DataSink& sink) -> bool {

                auto send_event = [&sink](const json& ev) -> bool {
                    std::string s = "data: " + ev.dump() + "\n\n";
                    return sink.write(s.data(), s.size());
                };

                auto intent = IntentRouter::parse(user_msg, last_sensor);

                json tool_result;
                std::string tool_text_for_llm;  // formatlanmis metin
                if (intent.is_tool) {
                    send_event({{"type","tool_call"},
                                {"name", intent.tool_name},
                                {"args", intent.args}});

                    tool_result = dispatcher.execute(intent.tool_name, intent.args);

                    // UI'a JSON gonderiyoruz (zengin gosterim icin)
                    send_event({{"type","tool_result"},
                                {"name", intent.tool_name},
                                {"result", tool_result}});

                    // LLM'e formatli metin gonderiyoruz
                    tool_text_for_llm = dispatcher.format_for_llm(intent.tool_name, tool_result);

                    if (intent.args.contains("sensor")) {
                        std::string s = intent.args["sensor"].get<std::string>();
                        if (s != "all") {
                            std::lock_guard<std::mutex> lk(g_ctx_mtx);
                            g_last_sensor = s;
                        }
                    }
                }

                std::string sys_prompt, user_for_llm;
                if (intent.is_tool) {
                    sys_prompt = SYSPROMPT_INTERPRET_TR;
                    user_for_llm =
                        "Soru: " + user_msg + "\n"
                        "Veri:\n" + tool_text_for_llm + "\n"
                        "Cevap:";
                } else {
                    sys_prompt = SYSPROMPT_CHAT_TR;
                    user_for_llm = user_msg;
                }

                json payload = {
                    {"model", "local"},
                    {"messages", {
                        {{"role","system"}, {"content", sys_prompt}},
                        {{"role","user"},   {"content", user_for_llm}}
                    }},
                    {"stream",       true},
                    {"temperature",  0.2},
                    {"max_tokens",   300},
                    {"cache_prompt", true},
                    {"stop", {"\n\nSoru:", "---Ornek", "Soru:"}}
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
    std::cout << "Chat: IntentRouter + Analyzer + Trend + LLM-as-Interpreter (10 few-shot)\n";
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