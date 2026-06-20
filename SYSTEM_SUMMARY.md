# Sensior — Ayrıntılı Sistem Özeti

> **Platform:** Raspberry Pi 4B / macOS (Apple Silicon)  
> **Dil:** C++17 · CMake · httplib · nlohmann/json  
> **LLM:** Qwen2.5-1.5B-Instruct Q4_K_M (llama.cpp / llama-server)  
> **Dal:** `real_sim` · Ana dal: `main`

---

## İçindekiler

1. [Projeye Genel Bakış](#1-projeye-genel-bakış)  
2. [Donanım Bileşenleri](#2-donanım-bileşenleri)  
3. [Yazılım Mimarisi](#3-yazılım-mimarisi)  
4. [SensorManager](#4-sensormanager)  
5. [IntentRouter](#5-intentrouter)  
6. [ToolDispatcher](#6-tooldispatcher)  
7. [Analyzer](#7-analyzer)  
8. [Dashboard HTTP Sunucusu](#8-dashboard-http-sunucusu)  
9. [LLM Entegrasyonu](#9-llm-entegrasyonu)  
10. [Web Arayüzü](#10-web-arayüzü)  
11. [API Endpointleri](#11-api-endpointleri)  
12. [Simülasyon Modu](#12-simülasyon-modu)  
13. [Yapılandırma Yönetimi](#13-yapılandırma-yönetimi)  
14. [LLM Değerlendirme Metrikleri](#14-llm-değerlendirme-metrikleri)  
15. [Platform Karşılaştırması: RPi4 vs Mac](#15-platform-karşılaştırması-rpi4-vs-mac)  
16. [Hata Analizi ve Zayıf Noktalar](#16-hata-analizi-ve-zayıf-noktalar)  
17. [Bağımlılıklar ve Derleme](#17-bağımlılıklar-ve-derleme)  
18. [Bilinen Kısıtlamalar ve Tasarım Kararları](#18-bilinen-kısıtlamalar-ve-tasarım-kararları)  
19. [Sonuç](#19-sonuç)

---

## 1. Projeye Genel Bakış

Sensior, düşük güçlü gömülü sistemler için tasarlanmış **doğal dil tabanlı sensör izleme asistanı**dır. Proje iki temel hedefi bir arada karşılar:

- **Gerçek zamanlı sensör izleme:** I²C üzerinden BME280, MPU6500 ve QMC5883L sensörlerinden sürekli veri toplama, eşik analizi ve anomali tespiti.
- **Doğal dil arayüzü:** Kullanıcının Türkçe veya İngilizce sorularına, yerel çalışan küçük bir LLM (Qwen2.5-1.5B) aracılığıyla kısa ve doğru yanıtlar üretme.

### Öne Çıkan Özellikler

| Özellik | Açıklama |
|---------|----------|
| Çift mod | `real` (I²C + donanım) ve `sim` (yazılımsal sensör) aynı kod tabanında |
| Hibrit NLP | Regex tabanlı IntentRouter + LLM; küçük model zafiyetini kapatır |
| SSE akışı | Token bazlı gerçek zamanlı LLM yanıt akışı tarayıcıya |
| Araç bypass | `POST /api/tool` ile LLM olmadan doğrudan ayar değiştirme |
| Halka tampon | Her sensör için `deque<SensorReading>` (max 6000 öğe), sabit bellek |
| Gizlilik | İnternet bağlantısı yok; tüm çıkarım yerel |

---

## 2. Donanım Bileşenleri

### 2.1 Raspberry Pi 4B

- **İşlemci:** Broadcom BCM2711 — 4× Cortex-A72 @ 1,8 GHz (64-bit ARMv8)
- **RAM:** 4 GB LPDDR4-3200
- **İşletim Sistemi:** Raspberry Pi OS 64-bit (Debian Bookworm tabanlı)
- **I²C:** `/dev/i2c-1` (GPIO2=SDA, GPIO3=SCL)
- **GPIO Güç Pini:** BCM-13 — sensör güç regülatörünü kontrol eder (active-high, 200 ms settle)

### 2.2 BME280 — Çevre Sensörü

| Parametre | Değer |
|-----------|-------|
| I²C Adresi | `0x76` |
| Sıcaklık aralığı | −40…+85 °C (±0,5 °C) |
| Nem aralığı | 0…100 % (±3 %) |
| Basınç aralığı | 300…1100 hPa (±1 hPa) |
| Güç tüketimi | ~3,6 µA (ölçüm modunda) |
| Varsayılan örnekleme | 1 Hz (yapılandırılabilir 1–50 Hz) |

**Çıktı alanları:** `temperature_c`, `humidity_pct`, `pressure_hpa`

### 2.3 MPU6500 — Atalet Ölçüm Birimi (IMU)

| Parametre | Değer |
|-----------|-------|
| I²C Adresi | `0x68` |
| İvmeölçer aralığı | ±2 / ±4 / ±8 / ±16 g |
| Jiroskop aralığı | ±250 / ±500 / ±1000 / ±2000 °/s |
| ADC çözünürlüğü | 16-bit |
| Varsayılan örnekleme | 50 Hz (yapılandırılabilir 1–200 Hz) |

**Çıktı alanları:** `accel_x`, `accel_y`, `accel_z` (g), `gyro_x`, `gyro_y`, `gyro_z` (°/s), `temp_c` (iç sıcaklık)

**Statik beklenti:** Z ekseninde ≈ 1 g yerçekimi, gyro eksenleri ≈ 0 °/s.

### 2.4 QMC5883L — Manyetometre / Pusula

| Parametre | Değer |
|-----------|-------|
| I²C Adresi | `0x0D` |
| Ölçüm aralığı | ±8 Gauss (3 eksen) |
| Çözünürlük | 1 mGauss |
| Varsayılan örnekleme | 10 Hz (yapılandırılabilir 1–20 Hz) |

**Çıktı alanları:** `mag_x`, `mag_y`, `mag_z` (Gauss), `heading_deg` (0–360°)

**Heading hesabı:** `atan2(mag_y, mag_x)` → derece, 0–360 ° aralığına normalize edilir.

---

## 3. Yazılım Mimarisi

```
┌─────────────────────────────────────────────────────────┐
│                    dashboard (binary)                    │
│                                                         │
│  SensorManager ──► Analyzer                            │
│       │                │                               │
│       ▼                ▼                               │
│  ToolDispatcher ◄──── Dashboard HTTP ◄──► IntentRouter │
│       │                │                               │
│       │                ▼                               │
│       │          llama-server (:8080)                  │
│       │                │                               │
│       └────────────────┼──────────────► Web UI (:8081) │
└────────────────────────┼────────────────────────────────┘
                         │ SSE / JSON
                    Tarayıcı (Chrome/Firefox)
```

### 3.1 Modül Tablosu

| Modül | Dosya(lar) | Sorumluluk |
|-------|-----------|------------|
| `SensorManager` | `sensor_manager.cpp/.hpp` | I²C okuma, ring buffer, thread yönetimi |
| `IntentRouter` | `intent_router.cpp/.hpp` | Regex parse, Intent üretimi |
| `ToolDispatcher` | `tool_dispatcher.cpp/.hpp` | 7 araç implementasyonu, LLM için formatlama |
| `Analyzer` | `analyzer.cpp/.hpp` | Eşik karşılaştırma, anomali skoru |
| `Dashboard` | `dashboard.cpp` | HTTP sunucu, SSE, chat mantığı, API |
| Sensör sürücüleri | `sensors/bme280.hpp` vb. | I²C protokol implementasyonu |
| Sim sensörler | `sensors/sim_*.hpp` | Sinüs + Gaussian gürültü modeli |
| `GPIOPower` | `gpio_power.hpp` | libgpiod ile güç pini kontrolü |
| `I2CBus` | `sensors/i2c_bus.hpp` | `/dev/i2c-1` bağlantı yönetimi |

### 3.2 İş Parçacığı Modeli

| Thread | Görev | Periyot |
|--------|-------|---------|
| Ana thread | HTTP sunucu (httplib blocking) | — |
| `SensorManager::worker_` | Sensör okuma döngüsü | ~100 ms tick, sensör rate'e göre |
| SSE handler thread | `/api/sensors/stream` her bağlantı | 200 ms |

Mutex: `SensorManager::mtx_` — worker thread ve HTTP handler'lar arası veri koruma.

---

## 4. SensorManager

### 4.1 Veri Yapısı

```cpp
struct SensorReading {
    int64_t timestamp_ms;   // system_clock epoch milisaniye
    nlohmann::json data;    // sensöre özgü alanlar
};

struct SensorInfo {
    Sensor*  sensor;        // polimorfik (real veya sim)
    int      rate_hz;       // mevcut örnekleme frekansı
    bool     enabled;       // runtime aktif/pasif bayrağı
    steady_clock::time_point next_sample;
    std::deque<SensorReading> history;  // max HISTORY_MAX = 6000
};

std::unordered_map<std::string, SensorInfo> sensors_;
```

### 4.2 Halka Tampon Kapasitesi

| Sensör | Rate (Hz) | HISTORY_MAX | Karşılık (sn) |
|--------|-----------|-------------|--------------|
| BME280 | 1–50 | 6000 | 120–6000 s |
| MPU6500 | 1–200 | 6000 | 30–6000 s |
| QMC5883L | 1–20 | 6000 | 300–6000 s |

### 4.3 `run_loop()` Algoritması

```
loop (running_ == true):
    lock(mtx_)
    for each (name, info) in sensors_:
        if !info.enabled: continue
        if now >= info.next_sample:
            data = info.sensor->read()
            if !data.empty():
                info.history.push_back({now_ms(), data})
                if history.size() > HISTORY_MAX:
                    history.pop_front()
            info.next_sample = now + 1000ms / rate_hz
    unlock(mtx_)
    sleep_until(min(next_sample across all sensors))
```

### 4.4 Kritik Metodlar

- `latest_all()` — tüm sensörlerin en son okuması; disabled sensör için `{online:false, enabled:false, data:null}`
- `history(name, seconds)` — son N saniyenin zaman damgalı okuma listesi
- `latest_n(name, count)` — son N ham okumanın array'i (max 200)
- `set_sample_rate(name, hz)` — runtime rate değişikliği; `sensor->set_rate(hz)` çağırır
- `set_enabled(name, bool)` — runtime aktif/pasif; config dosyasına ayrıca `ToolDispatcher` yazar

---

## 5. IntentRouter

### 5.1 Görev

`parse(user_message) → Intent{is_tool, tool_name, args}` döndürür. LLM çağrısı olmadan deterministik yönlendirme sağlar.

### 5.2 `Intent` Yapısı

```cpp
struct Intent {
    bool        is_tool   = false;
    std::string tool_name;          // "get_current", "set_threshold" vb.
    nlohmann::json args   = {};
};
```

### 5.3 Normalize Adımı

```cpp
std::string normalize(const std::string& msg):
    küçük harfe çevir
    TR karakter dönüşümü: ş→s, ı→i, ğ→g, ü→u, ö→o, ç→c
    return norm
```

### 5.4 `is_technical_question()` — Erken Çıkış Mantığı

`force_tech = true` olursa `intent.is_tool = false` zorlanır. Üç regex katmanlı:

1. **`data_query_re`** (erken çıkış — tool bırakılır):  
   `right now | current | live | real-time | latest | reading | measure | value now`  
   + metrik adları: `temperature | humidity | pressure | heading | compass | bearing | acceleration | gyro | magnetic`

2. **`info_re`** (teknik soru işareti):  
   `what is | how does | explain | tell me about | define | describe`

3. **`tech_topic_re`** (teknik konu doğrulama):  
   `i2c | spi | sensor | bme | mpu | qmc | accelerometer | gyroscope | magnetometer | gguf | llm | quantization | ...`

Kural: `info_re` ∧ `tech_topic_re` ∧ ¬`data_query_re` → `force_tech = true`

### 5.5 Yönlendirme Zinciri

```
1. detect_sensor(norm)   → "bme280" | "mpu6050" | "qmc5883l" | ""
2. detect_metric(norm)   → "temperature_c" | "heading_deg" | ...
3. has_set_keyword(norm) → SET mi GET mi?

SET dalı:
  ├─ "enabled" / "disabled"   → set_sensor_enabled
  ├─ "hz" / "rate"            → set_sample_rate
  └─ eşik kelimesi            → set_threshold
        ├─ min/max/üst/alt yönü tespit
        └─ metric_key fallback (sensör adından tahmin)

GET dalı:
  ├─ "config" / "settings" / "parameters"  → get_config
  ├─ "last N sec/min" kalıbı               → get_history_stats veya get_history_raw
  │     └─ "raw" / "readings" / "list"     → get_history_raw (count=N)
  └─ varsayılan                             → get_current
        └─ sensor="all" eğer sensör tespit edilemezse
```

### 5.6 `detect_negative()`

Olmayan sensörlere atıf tespit eder → LLM'e `negative_sysprompt` beslenir:

- **Olmayan sensörler:** `air quality | co2 | pm2.5 | light | lux | noise | decibel | distance | gps | uv | ...`
- **Genel sağlık soruları:** `is it safe | dangerous | healthy | harmful | ...`
- **Kapsam dışı:** `lottery | stock | weather forecast | horoscope | ...`

---

## 6. ToolDispatcher

### 6.1 Araç Kataloğu

| Araç | Argümanlar | Açıklama |
|------|-----------|----------|
| `get_current` | `sensor: string` | Anlık okuma + anomali analizi |
| `get_history_stats` | `sensor, metric, seconds: int` | avg / min / max / trend / anomali sayısı |
| `get_history_raw` | `sensor, count: int (max 200)` | Son N okuma, zaman damgalı |
| `get_config` | — | Tüm yapılandırılabilir parametreler |
| `set_threshold` | `metric: string, min?: float, max?: float` | Anomali eşiği güncelle + kaydet |
| `set_sample_rate` | `sensor: string, hz: int` | Örnekleme frekansı değiştir + kaydet |
| `set_sensor_enabled` | `sensor: string, enabled: bool` | Sensörü aç/kapat (runtime + kalıcı) |

### 6.2 `set_sensor_enabled` Detayı

İki katmanlı işlem:
1. `config.json` dosyasını günceller (`cfg["sensors"][sensor]["enabled"] = enabled`)
2. `sensors_.set_enabled(sensor, enabled)` ile runtime `SensorInfo::enabled` bayrağını değiştirir

Bu iki adım zorunludur: yalnızca config güncellemek yeniden başlatmadan önce etkisizdir.

### 6.3 `format_for_llm()` Çıktı Yapısı

```
Sensor data for bme280:
  Temperature: 24.3°C  [OK — within 15.0–38.0°C]
  Humidity: 58.0%      [OK — within 20.0–80.0%]
  Pressure: 1013.2 hPa [OK]

No anomalies detected.
```

Anomali varsa:
```
  Temperature: 39.5°C  ⚠ ABOVE MAX threshold (38.0°C)

1 anomaly detected.
```

### 6.4 Güvenlik Kapısı

`POST /api/tool` endpoint'inde izin verilen araçlar sabit kümesi:

```cpp
static const std::unordered_set<std::string> ALLOWED = {
    "set_threshold", "set_sample_rate", "set_sensor_enabled"
};
// Bu küme dışındaki istek → HTTP 403
```

---

## 7. Analyzer

- **`check_thresholds(sensor, data, config)`:** Her metrik için `min` / `max` sınırı ile karşılaştırır, aşılan eşiklerin listesini döndürür.
- **`anomaly_score(history, config)`:** Son N okuma üzerinde eşik ihlali oranını hesaplar; `0.0` (ihlal yok) – `1.0` (tüm okumalar ihlal).

Config'deki eşik yapısı:

```json
"thresholds": {
  "bme280.temperature_c": { "min": 15.0, "max": 38.0 },
  "bme280.humidity_pct":  { "min": 20.0, "max": 80.0 },
  "mpu6050.accel_z":      { "min": 0.85, "max": 1.15 }
}
```

---

## 8. Dashboard HTTP Sunucusu

### 8.1 Genel Yapı

`cpp-httplib` (tek başlık dosyası) kullanan senkron HTTP sunucusu. Port: `8081`. Her SSE bağlantısı ayrı thread'de çalışır.

### 8.2 Chat İşleme Pipeline

```
handle_chat(req, res):
  1. JSON body parse → user_msg, history
  2. Ack bypass kontrolü     → "ok|got it|sure" → hızlı yanıt, dön
  3. normalize(user_msg)
  4. detect_negative()       → force_negative = true/false
  5. is_technical_question() → force_tech = true/false
  6. IntentRouter::parse()   → Intent
  7. intent.is_tool && !force_tech:
       ToolDispatcher::execute() → tool_result
       user_for_llm = "Question: " + msg + "\nData (authoritative):\n" + tool_text
       sysprompt = SYSPROMPT_SENSOR
  8. else if force_tech:
       sysprompt = SYSPROMPT_TECH
  9. else:
       sysprompt = force_negative ? negative_sysprompt : SYSPROMPT_CHAT
 10. max_tok hesapla (120–500, intent'e göre)
 11. llama-server SSE isteği → token'ları SSE olarak tarayıcıya ilet
 12. Sohbet geçmişine ekle (MAX_HISTORY_MSGS = 8 ile kırp)
```

### 8.3 `max_tok` Mantığı

```cpp
int max_tok = is_sensor_mode ? 120 : 160;
if (intent.tool_name == "get_history_raw") max_tok = 500;
if (intent.tool_name == "get_config")      max_tok = 350;
if (intent.args.value("sensor","") == "all") max_tok = 300;
```

### 8.4 System Promptları

**`SYSPROMPT_SENSOR`:**
> You are a sensor monitoring assistant. Reply in 1–2 English sentences using only the provided data. Report the exact numbers — never fabricate values. If data is present, the sensor IS active — NEVER say disabled when data is shown. Ignore prior conversation suggesting a sensor was disabled.

**`SYSPROMPT_CHAT`:**
> You are a sensor assistant. Help the user interact with BME280, MPU6500, QMC5883L sensors. Valid commands: `set bme280 sample rate 20 hz` / `set bme280 temperature threshold max 35` / `set bme280 enabled` / `set bme280 disabled`. Keep answers under 3 sentences.

**`SYSPROMPT_TECH`:**
> You are a sensor electronics expert. Answer technical questions about sensors, I2C protocol, MEMS, GGUF quantization, and embedded systems. Be concise (2–3 sentences).

---

## 9. LLM Entegrasyonu

### 9.1 Model

| Parametre | Değer |
|-----------|-------|
| Model | Qwen2.5-1.5B-Instruct |
| Nicemleme | Q4_K_M (GGUF) |
| Boyut | ~1,0 GB |
| Bağlam | 4096 token |
| Sohbet şablonu | ChatML (`<\|im_start\|>` / `<\|im_end\|>`) |
| Thinking modu | `enable_thinking: false` (Qwen3 uyumluluğu için) |

### 9.2 llama-server Parametreleri

```bash
llama-server \
  -m models/qwen2.5-1.5b-instruct-q4_k_m.gguf \
  -c 4096 \
  --cache-type-k q8_0 \
  --jinja \
  --chat-template-kwargs '{"enable_thinking": false}' \
  --port 8080
```

### 9.3 Neden LLM Function Calling Kullanılmadı

Qwen2.5-1.5B, `tool_calls` alanı yerine JSON'u düz metin kod bloğu olarak üretmektedir. Test sürecinde gözlemlenen davranışlar:

- Araç çağrısı yerine `{"tool": "get_current", "args": {...}}` içerikli Markdown kod bloğu üretimi
- `tool_calls` alanının hiç doldurulmaması
- Yapılandırılmış çıktı tutarsızlığı

**Karar:** LLM function calling kaldırıldı, IntentRouter korundu. Daha büyük model (≥7B) veya function calling fine-tune gerektirir.

### 9.4 Bağlam Penceresi Bütçesi (4096 token)

| İstek Tipi | System Prompt | Tool Çıktısı | Sohbet Geçmişi | LLM Yanıtı | Kalan |
|-----------|--------------|-------------|----------------|------------|-------|
| `get_current` | 95 | 180 | 210 | 90 | 3521 |
| `get_history (30s)` | 95 | 320 | 210 | 110 | 3361 |
| `get_history_raw (10)` | 95 | 850 | 210 | 180 | 2661 |
| `set_threshold` | 95 | 120 | 210 | 70 | 3601 |
| `get_config` | 95 | 420 | 210 | 140 | 3231 |
| `tech_question` | 110 | 0 | 210 | 210 | 3566 |
| `chat (dolu geçmiş)` | 95 | 0 | 650 | 120 | 3231 |

`MAX_HISTORY_MSGS = 8` ile geçmiş kırpılır, taşma önlenir.

### 9.5 Örnekleme Parametreleri

```cpp
// Sensör modu (deterministik sayılar için düşük sıcaklık)
temperature = 0.2, top_p = 0.9

// Sohbet / teknik modu (daha yaratıcı)
temperature = 0.5, top_p = 0.9
```

---

## 10. Web Arayüzü

### 10.1 Teknoloji Yığını

Saf HTML5 / CSS3 / Vanilla JavaScript — framework bağımlılığı yok. Chart.js 4.x grafikler için. `webui/` dizinindeki dosyalar `dashboard` binary tarafından statik olarak sunulur.

### 10.2 Sayfa Düzeni

CSS Grid iki sütunlu yapı:

```
┌──────────────────┬────────────────────┐
│  Sol Sütun       │  Sağ Sütun         │
│                  │                    │
│  BME280 Kartı    │  Chat Arayüzü      │
│  MPU6500 Kartı   │  (SSE EventSource) │
│  QMC5883L Kartı  │                    │
│  ⚙ Settings      │  Örnek Chip'ler    │
│    Paneli        │  Composer Textarea │
└──────────────────┴────────────────────┘
```

### 10.3 Gerçek Zamanlı Sensör Güncellemesi

`EventSource('/api/sensors/stream')` ile 200 ms'de bir tüm sensör verileri alınır. Chart.js ring buffer modeli: son 60 veri noktası grafikte gösterilir, eskiler otomatik silinir.

### 10.4 Örnek Soru Chip'leri

```javascript
const EXAMPLE_PROMPTS = [
  { icon: "🌡", label: "Temperature",    text: "What is the current temperature?" },
  { icon: "💧", label: "Humidity",       text: "What is the current humidity?" },
  { icon: "📊", label: "30s Trend",      text: "Show last 30 seconds trend for bme280" },
  { icon: "🔢", label: "Last 10 reads",  text: "Show last 10 readings of bme280" },
  { icon: "⚠️", label: "Anomalies",      text: "Are there any anomalies?" },
  { icon: "📡", label: "All sensors",    text: "Show all sensor readings" },
  { icon: "🧭", label: "Heading",        text: "What is the compass heading?" },
  { icon: "⚙",  label: "What can I set", text: "What can I change?" },
];
```

### 10.5 Ayarlar Paneli

`GET /api/config` ile yapılandırma çekilir, her sensör için dinamik tab oluşturulur:

- **Örnekleme hızı kaydırıcısı** (`range` input) — `set_sample_rate` aracı tetikler
- **Eşik tablosu** (min/max sayısal input) — `set_threshold` aracı tetikler
- **Aktif/Pasif toggle** — `set_sensor_enabled` aracı tetikler
- **Geri bildirim:** Başarı → yeşil ✓ flash, hata → kırmızı ✗ flash (CSS animasyonu)

### 10.6 SSE Chat Akışı

```javascript
// Gönderme
const res = await fetch('/api/chat', {
  method: 'POST',
  body: JSON.stringify({ message: userMsg, history: chatHistory })
});
const reader = res.body.getReader();
// Token'ları satır satır oku, chat baloncuğuna ekle
```

---

## 11. API Endpointleri

### Tam Endpoint Listesi

| Endpoint | Yöntem | Açıklama |
|----------|--------|----------|
| `/` | GET | `webui/index.html` statik servis |
| `/style.css` | GET | Stil dosyası |
| `/app.js` | GET | JavaScript |
| `/api/health` | GET | `{"status":"ok"}` |
| `/api/sensors/stream` | GET | SSE — 200 ms'de tüm sensör JSON |
| `/api/chat` | POST | SSE akışlı LLM chat |
| `/api/config` | GET | Yapılandırma JSON |
| `/api/tool` | POST | Direkt araç çağrısı (LLM bypass) |
| `/api/mode` | GET/POST | `sim`↔`real` mod geçişi |
| `/api/translator` | GET | LibreTranslate entegrasyon durumu |

### `/api/chat` İstek / Yanıt

```json
// İstek
{
  "message": "What is the current temperature?",
  "history": [
    {"role": "user",      "content": "..."},
    {"role": "assistant", "content": "..."}
  ]
}

// Yanıt: SSE stream
data: {"delta": "The "}
data: {"delta": "current "}
data: {"delta": "temperature "}
data: {"delta": "is 24.3°C."}
data: [DONE]
```

### `/api/tool` İstek / Yanıt Örnekleri

```json
// Örnekleme hızı
POST /api/tool
{"name": "set_sample_rate", "args": {"sensor": "bme280", "hz": 20}}
→ {"hz_applied": 20, "ok": true, "persisted": true, "sensor": "bme280"}

// Eşik güncelleme
POST /api/tool
{"name": "set_threshold", "args": {"metric": "bme280.temperature_c", "min": 15, "max": 35}}
→ {"changed": {"max": {"new": 35, "old": 38}}, "ok": true, "persisted": true}

// Sensör devre dışı bırakma
POST /api/tool
{"name": "set_sensor_enabled", "args": {"sensor": "mpu6050", "enabled": false}}
→ {"enabled": false, "ok": true, "persisted": true, "sensor": "mpu6050"}
```

### `/api/mode` Geçiş Mekanizması

`EXIT_CODE_RESTART = 42` ile süreç çıkar, `start_server.sh` bu kodu yakalar ve `dashboard` binary'sini yeni modda yeniden başlatır:

```bash
while true; do
  ./dashboard --config config.mac.json
  EXIT=$?
  [ $EXIT -eq 42 ] || break
done
```

---

## 12. Simülasyon Modu

### 12.1 Sim Sensör Modeli

Her sim sensör `Sensor` abstract sınıfından türetilir, aynı `read()` arayüzüne sahiptir.

**SimBME280:**
```cpp
double t_sec = elapsed_seconds();
temperature = 22.0 + 4.0 * sin(t_sec / 10.0) + normal(0, 0.12);
humidity    = 55.0 + 8.0 * sin(t_sec / 15.0 + 1.0) + normal(0, 0.4);
pressure    = 1013.25 + 2.0 * sin(t_sec / 30.0) + normal(0, 0.1);
```

**SimMPU6050:**
```cpp
accel_z = 0.99 + 0.01 * sin(t_sec / 7.0) + normal(0, 0.008);
// X, Y: ~0g; gyro tüm eksenler: ~0 °/s + gürültü
```

**SimQMC5883L:**
```cpp
heading = fmod(180.0 + 30.0 * sin(t_sec / 20.0), 360.0) + normal(0, 1.5);
```

### 12.2 Mod Farkı

| Özellik | Real Mod | Sim Mod |
|---------|----------|---------|
| I²C erişimi | `open("/dev/i2c-1")` | Yok (nullptr) |
| GPIO güç pini | BCM-13 kontrol | Yok |
| Sensör init | Fiziksel register yazma | Anlık `true` döner |
| Veri kaynağı | Gerçek donanım | Matematiksel model |
| Geliştirme | RPi4 gerekli | Mac/herhangi PC |

---

## 13. Yapılandırma Yönetimi

### 13.1 Config Dosyaları

| Dosya | Platform | Kullanım |
|-------|----------|---------|
| `config.mac.json` | macOS | Geliştirme ve demo |
| `config.rpi.json` | RPi4 | Üretim |

### 13.2 Config Yapısı

```json
{
  "mode": "sim",
  "port": 8081,
  "llm": {
    "endpoint": "http://localhost:8080",
    "model": "qwen2.5-1.5b-instruct-q4_k_m"
  },
  "sensors": {
    "bme280": {
      "enabled": true,
      "sample_rate_hz": 1
    },
    "mpu6050": {
      "enabled": true,
      "sample_rate_hz": 50
    },
    "qmc5883l": {
      "enabled": true,
      "sample_rate_hz": 10
    },
    "power_pin": {
      "bcm": 13,
      "active_high": true,
      "chip": "gpiochip0",
      "settle_ms": 200
    }
  },
  "thresholds": {
    "bme280.temperature_c": { "min": 15.0, "max": 38.0 },
    "bme280.humidity_pct":  { "min": 20.0, "max": 80.0 },
    "bme280.pressure_hpa":  { "min": 900.0, "max": 1100.0 },
    "mpu6050.accel_z":      { "min": 0.85, "max": 1.15 },
    "mpu6050.temp_c":       { "min": 20.0, "max": 60.0 },
    "qmc5883l.heading_deg": { "min": 0.0, "max": 360.0 }
  }
}
```

---

## 14. LLM Değerlendirme Metrikleri

40 test senaryosu (9 intent kategorisi) üzerinde `confident-ai.com` metrik çerçevesiyle değerlendirme yapılmıştır.

### 14.1 Genel Sonuçlar

| Metrik | Skor | Yorumlama |
|--------|------|-----------|
| **Faithfulness** | 0.825 (82.5%) | LLM sensör verisine büyük ölçüde sadık |
| **Tool Correctness** (araç gerektiren, n=33) | 0.939 (93.9%) | IntentRouter güvenilir yönlendirir |
| **Argument Correctness** (n=33) | 0.848 (84.8%) | Araç doğru, argüman bazen eksik |
| **Task Completion** (uçtan uca) | ~0.86 | Makul; set_* komutları sürükler |
| **Tech/Chat araç çağırmama** (n=7) | 1.000 (100%) | Teknik sorular hiç araç çağırmadı |
| **BLEU** (ortama) | 0.227 | Düşük — kısa GT vs uzun yanıt (metot uyumsuz) |
| **ROUGE-1** (ortalama) | 0.655 | Daha anlamlı; GT kelimeleri yanıtta mevcut |
| **G-Eval** (1–5, ortalama) | ~4.1 | "İyi" bandında |

### 14.2 Intent Bazında Sonuçlar

| Intent | n | Faithfulness | Tool OK | Arg OK |
|--------|---|-------------|---------|--------|
| `get_current` | 12 | 10/12 | 12/12 | 12/12 |
| `get_history` | 6 | 5/6 | 6/6 | 5/6 |
| `get_history_raw` | 2 | 2/2 | 2/2 | 2/2 |
| `set_threshold` | 4 | 2/4 | 3/4 | 2/4 |
| `set_sample_rate` | 3 | 2/3 | 3/3 | 2/3 |
| `set_enabled` | 4 | 3/4 | 3/4 | 3/4 |
| `get_config` | 2 | 2/2 | 2/2 | 2/2 |
| `tech_question` | 3 | 3/3 | 0/3* | 0/3* |
| `chat` | 4 | 4/4 | 0/4* | 0/4* |

*Araç çağırmaması beklenen ve doğru davranış.

### 14.3 Sensör Korelasyon Bulguları

600 okuma üzerinde Pearson korelasyonu:

| Çift | r | Yorum |
|------|---|-------|
| BME280 Temp ↔ Nem | −0.72 | Güçlü negatif — sıcaklık arttıkça nem düşer |
| BME280 Temp ↔ Basınç | +0.41 | Orta pozitif |
| BME280 Temp ↔ MPU6500 İç Temp | +0.89 | Güçlü pozitif — çevre ısınca IMU ısınıyor |
| MPU6500 aZ ↔ Gyro | −0.08 | Zayıf — statik cihazda beklenen |
| QMC Heading ↔ diğerleri | ~0.0 | Bağımsız — beklenen |

---

## 15. Platform Karşılaştırması: RPi4 vs Mac

### 15.1 LLM Yanıt Hızı (CPU-only, Qwen2.5-1.5B Q4_K_M)

| Metrik | Raspberry Pi 4B | Mac M-serisi | Oran |
|--------|----------------|-------------|------|
| Token/saniye | ~3.8 tok/sn | ~24.0 tok/sn | 6.3× |
| İlk token gecikmesi | ~4.8 sn | ~0.85 sn | 5.6× |
| RAM kullanımı (model) | ~950 MB | ~990 MB | ≈ eşit |
| CPU yük (çıkarım) | ~88% (4 çekirdek) | ~76% | RPi daha yüklü |
| Faithfulness skoru | 0.825 | 0.825 | Aynı (model aynı) |

### 15.2 Intent Bazında RPi4 Yanıt Süresi (medyan, saniye)

| Intent | Medyan | %25 | %75 |
|--------|--------|-----|-----|
| `get_current` | 4.8 | 4.4 | 5.3 |
| `get_history` | 6.5 | 5.8 | 7.1 |
| `get_history_raw` | 8.2 | 7.0 | 9.1 |
| `set_*` | 5.1 | 4.6 | 5.8 |
| `get_config` | 7.8 | 6.5 | 8.9 |
| `tech_question` | 11.4 | 9.5 | 12.8 |
| `chat/ack` | 0.05 | 0.03 | 0.07 |

### 15.3 RPi4 Sistem Kaynakları (2 dakika çalışma)

- **Token/sn eğrisi:** Warmup (ilk 15 sn) sonrası 3.2 → 4.2 tok/sn'ye yerleşir
- **RAM:** Sabit ~980 MB (model yüklendikten sonra artış minimumdur)
- **CPU:** Çıkarım spike'larında %88–99, bekleme döneminde ~%8–15

### 15.4 RPi4'e Özgü Optimizasyonlar

- `--cache-type-k q8_0` — KV cache nicemleme ile RAM tasarrufu
- `-c 4096` — maksimum bağlam sınırı (daha büyük = daha fazla RAM)
- `MAX_HISTORY_MSGS = 8` — geçmiş kırpma ile bağlam taşması önleme
- Düşük örnekleme hızı varsayılanları — sensör CPU yükünü minimize eder

---

## 16. Hata Analizi ve Zayıf Noktalar

### 16.1 Hata Taksonomisi (n=7 başarısız senaryo)

| Tip | Sayı | Örnek |
|-----|------|-------|
| **Hallüsinasyon** (yanlış sayı) | 2 | "around 30°C" → gerçek: 24.3°C |
| **Muğlak yanıt** (sayı eksik) | 3 | "Threshold has been changed." → 35°C belirtilmedi |
| **Yanlış araç seçimi** | 1 | Settings panel önerildi, araç çağrılmadı |
| **Yanlış durum bildirimi** | 1 | "The sensor is not enabled." → enabled=true iken |

### 16.2 SYSPROMPT Etkisi (simüle karşılaştırma)

| Metrik | SYSPROMPT yok | SYSPROMPT aktif | İyileşme |
|--------|--------------|----------------|---------|
| Faithfulness | 0.58 | 0.83 | +43% |
| Answer Relevancy | 0.64 | 0.87 | +36% |
| Hallucination Oranı | 0.38 | 0.18 | −53% |
| Sayı Doğruluğu | 0.52 | 0.88 | +69% |

### 16.3 `set_*` Komutlarının Zayıflığı

4 `set_threshold` senaryosundan yalnızca 2'si tam başarılı (%50). Nedenler:

1. LLM bazen araç çağrısının ardından sayıyı tekrar etmek yerine genel cümle üretir
2. IntentRouter metric_key fallback'i bazen yanlış metriğe düşer
3. "Threshold has been changed." gibi muğlak yanıtlar faithful değil (sayı yok)

### 16.4 BLEU'nun Yetersizliği

BLEU, makine çevirisi için tasarlanmış precision-based bir metriktir. Kısa ground-truth (`"24.3°C, 58%"`) ile uzun doğal dil yanıtı (`"The current temperature is 24.3°C with 58% humidity."`) karşılaştırıldığında ekstra kelimeler ceza olarak hesaba katılır. ROUGE-1 recall-based olduğu için bu bağlamda daha anlamlıdır.

---

## 17. Bağımlılıklar ve Derleme

### 17.1 C++ Bağımlılıkları

| Kütüphane | Türü | Kullanım |
|-----------|------|---------|
| `nlohmann/json` 3.11+ | Header-only | JSON parse/serialize |
| `cpp-httplib` 0.14+ | Header-only | HTTP sunucu + SSE |
| `libgpiod` | Sistem | GPIO pin kontrolü (RPi4) |
| `i2c-tools` | Sistem | I²C bus erişimi (RPi4) |

### 17.2 CMake Hedefleri

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4          # RPi4'te ~90 sn, Mac'te ~8 sn
```

### 17.3 llama.cpp / llama-server

```bash
# Derleme (Mac)
cmake -B build -DLLAMA_METAL=ON    # Metal GPU offload (Mac)
# Derleme (RPi4)
cmake -B build                     # CPU-only

cmake --build build --config Release -j4
```

### 17.4 Python Araçları (rapor üretimi)

```bash
pip3 install reportlab matplotlib pillow
python3 generate_report.py   # → report_assets/ (20 PNG grafik)
```

---

## 18. Bilinen Kısıtlamalar ve Tasarım Kararları

### 18.1 LLM Seçimi

**Qwen2.5-1.5B neden?**  
RPi4'te 4–5 tok/sn, ~950 MB RAM, İngilizce talimat uyumu yeterli. Daha büyük modeller (3B, 7B) RPi4'te pratikte kullanılamaz derecede yavaş.

**Neden function calling yok?**  
1.5B model `tool_calls` alanı yerine düz metin JSON üretiyor. IntentRouter bu eksikliği deterministik regex ile kapatıyor.

### 18.2 Çift Mod Mimarisi

`sim` ve `real` aynı binary, yalnızca `SensorInfo::sensor` pointer'ı farklı nesneye işaret eder. Bu sayede:
- Donanımsız CI/CD testi mümkün
- Mac'te geliştirme, RPi4'te üretim
- `mode` config alanı ile çalışma zamanı geçişi

### 18.3 Neden httplib?

Tek başlık dosyası, C++17 uyumlu, SSE desteği var. Alternatif (Crow, Boost.Beast) daha ağır bağımlılık getirir. Proje ölçeğinde httplib yeterli.

### 18.4 IntentRouter vs Saf LLM Routing

| Yaklaşım | Avantaj | Dezavantaj |
|----------|---------|------------|
| IntentRouter (mevcut) | Deterministik, hızlı (<1 ms), güvenilir | Yeni intent için regex yazılmalı |
| Saf LLM routing | Esnek, doğal dil | 1.5B model güvenilmez, yavaş, RPi4'te 2× çağrı |

### 18.5 Geçmiş Kırpma

`MAX_HISTORY_MSGS = 8` (4 kullanıcı + 4 asistan turu). Daha uzun geçmiş: 1.5B model bağlam taşmasında halüsinasyon üretir. Daha kısa geçmiş: çok turlu diyalog bağlamı kaybolur.

---

## 19. Sonuç

Sensior, kısıtlı kaynaklara sahip gömülü bir sistemde (RPi4) yerel LLM entegrasyonunun pratikte uygulanabilirliğini göstermektedir. Projenin temel katkıları:

1. **Hibrit NLP mimarisi** — IntentRouter + LLM kombinasyonu, 1.5B parametreli modelin yapısal zafiyetini (function calling yetersizliği) başarıyla aşar. Tool correctness %93.9'a ulaşır.

2. **Bellek-sabit ring buffer** — `HISTORY_MAX = 6000` öğelik `deque`, yüksek frekanslı sensör verisini sabit RAM içinde saklar.

3. **Çift mod geliştirme** — Sim/real aynı kod tabanı, donanım bağımlılığı olmadan geliştirme döngüsünü kısaltır.

4. **SSE akışlı deneyim** — Token bazlı LLM yanıt iletimi, RPi4'ün ~4.8 saniyelik ilk yanıt gecikmesini kullanıcı açısından "canlı yazım" hissiyle örtbas eder.

5. **Doğrudan araç API'si** — `POST /api/tool` ile LLM bypass'ı, ayarlar panelini deterministik ve anlık yapar.

**En kritik bulgu:** Faithfulness %82.5, sistemin %17.5 oranında ya yanlış sayı ürettiğini ya da muğlak yanıt verdiğini göstermektedir. Bu oran, sensör izleme gibi sayı-kritik uygulamalar için sınırda kabul edilebilir; üretim ortamı için `user_for_llm` içindeki "authoritative data" vurgusunun ve SYSPROMPT_SENSOR kısıtlarının güçlendirilmesi önerilir.

**Gelecek yönler:**
- Qwen2.5-3B veya Phi-3-mini ile kalite/hız karşılaştırması
- MQTT ile çok cihazlı mimari
- Sensör drift otomatik kalibrasyonu
- Mobil uyumlu PWA arayüzü
- OpenAPI şeması ile dış entegrasyon
