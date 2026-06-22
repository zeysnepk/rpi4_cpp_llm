# Sensior

A natural-language sensor monitoring assistant for Raspberry Pi 4, written in C++17.  
Users interact via a web dashboard; a locally-running LLM (Qwen2.5-1.5B) answers questions about live sensor data in plain English.

---

## Features

- **Real-time sensor monitoring** — BME280 (temperature, humidity, pressure), MPU6500 (accelerometer, gyroscope), QMC5883L (magnetometer, heading) over I²C
- **Natural language interface** — ask questions like *"what's the temperature?"* or *"set humidity max to 70"*
- **Hybrid NLP** — regex-based intent routing handles tool calls reliably; LLM handles open-ended chat
- **SSE streaming** — token-by-token response delivery to the browser
- **Dual mode** — `real` (I2C hardware) and `sim` (software simulation) share the same codebase
- **Privacy** — fully local inference, no internet connection required

---

## Hardware

| Component | Role | I²C Address |
|-----------|------|-------------|
| Raspberry Pi 4B (4 GB) | Main compute | — |
| BME280 | Temperature / Humidity / Pressure | `0x76` |
| MPU6500 | Accelerometer / Gyroscope | `0x68` |
| QMC5883L | Magnetometer / Compass heading | `0x0D` |

GPIO BCM-13 controls the sensor power rail (active-high, 250 ms settle).

---

## Architecture

```
┌──────────────────────────────────────────────┐
│                  dashboard (C++)              │
│                                              │
│  SensorManager ──► Analyzer                 │
│       │                │                    │
│       ▼                ▼                    │
│  ToolDispatcher ◄── HTTP Server ◄── IntentRouter
│       │                │                    │
│       │          llama-server (:8080)        │
│       └──────────────────────► Web UI (:8081)│
└──────────────────────────────────────────────┘
```

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| `SensorManager` | `sensor_manager.*` | I²C polling, ring-buffer history (max 6000 samples/sensor) |
| `IntentRouter` | `intent_router.*` | Regex parse → `Intent{is_tool, tool_name, args}` |
| `ToolDispatcher` | `tool_dispatcher.*` | 7 tools: get current/history/stats/config, set threshold/rate/enable |
| `Analyzer` | `analyzer.*` | Threshold comparison, anomaly scoring |
| `Dashboard` | `dashboard.cpp` | HTTP server (httplib), SSE, chat logic, API handlers |
| Sim sensors | `sensors/sim_*.cpp` | Sinusoid + Gaussian noise model |
| `GPIOPower` | `gpio_power.*` | libgpiod power-pin control |

---

## Getting Started

### Prerequisites

- CMake ≥ 3.14, C++17 compiler
- `libgpiod-dev` (Linux/RPi only)
- [llama.cpp](https://github.com/ggerganov/llama.cpp) built with `llama-server`
- `Qwen2.5-1.5B-Instruct-Q4_K_M.gguf` model file

### RPi4 — First-time setup

```bash
git clone <repo-url>
cd <repo-folder>
bash setup_rpi.sh
```

This installs dependencies, builds the dashboard, builds `llama-server`, and checks for the model file.  
Copy the model from your Mac:
```bash
scp models/qwen2.5-1.5b-instruct-q4_k_m.gguf pi@<RPi-IP>:~/models/
```

### Mac — Simulation mode

```bash
cmake -B build
cmake --build build -j4
```

---

## Running

Two terminals are needed:

**Terminal 1 — LLM inference server**
```bash
./start_server.sh
```

**Terminal 2 — Dashboard**
```bash
# RPi (real hardware) or Mac (simulation):
./start_dashboard.sh          # wrapper with auto-restart on mode switch
```

Open `http://<device-ip>:8081` in a browser.

---

## Configuration

| File | Platform | Mode |
|------|----------|------|
| `config.mac.json` | macOS | `sim` |
| `config.rpi.json` | Raspberry Pi 4 | `real` |

Key fields:

```json
{
  "mode": "real",
  "llm": { "temperature": 0.2, "max_tokens": 200 },
  "thresholds": {
    "bme280.temperature_c": { "min": 15, "max": 32, "label": "room temperature" }
  },
  "sensors": {
    "bme280": { "enabled": true, "sample_rate_hz": 1, "i2c_address": "0x76" }
  }
}
```

Thresholds and sample rates can also be changed at runtime through the dashboard settings panel.

---

## API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/chat` | POST | Send a user message; returns SSE stream |
| `/api/sensors` | GET | Latest reading from all sensors (JSON) |
| `/api/sensors/stream` | GET | SSE stream, updates every 200 ms |
| `/api/tool` | POST | Direct tool execution (bypasses LLM) |
| `/api/config` | GET | Current config JSON |
| `/api/mode` | GET | Current mode (`real` / `sim`) |

---

## Sensor Test

```bash
i2cdetect -y 1
```

Expected addresses: `0x76` (BME280), `0x68` (MPU6500), `0x0D` (QMC5883L).

---

## Dependencies

All third-party headers are bundled in `third_party/` — no separate installation needed.

| Library | Version | License |
|---------|---------|---------|
| [httplib](https://github.com/yhirose/cpp-httplib) | single header | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | single header | MIT |

---

## License

MIT
