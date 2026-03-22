# OpenTrackFit - Development Guide

## IMPORTANT: Public Repository
This is a **public open-source repository**. Never commit sensitive data such as passwords, API keys, tokens, IP addresses, hostnames, or any personal credentials. All user-specific configuration (WiFi, MQTT, HTTP webhook, profiles) is entered at runtime and stored in NVS on the ESP32 — not in source code. When making changes, always verify that no sensitive information is hardcoded or accidentally included.

## Project Overview
ESP32 firmware (PlatformIO/Arduino) that reads weight data from a FitTrack BLE body composition scale and serves it via a web interface. Supports MQTT and HTTP webhook forwarding.

## Build & Flash
```bash
pio run                # Build
pio run -t upload      # Flash to ESP32
pio device monitor     # Serial monitor (close before flashing!)
```

## Architecture
Single-file firmware (`src/main.cpp`) with these subsystems:
- **BLE**: Non-blocking scan for "FitTrack" scale, connects, subscribes to FFB2/FFB3 notifications
- **WiFi**: STA mode with AP fallback (config portal with SSID scan)
- **WebServer**: Weight display page (STA) / WiFi+MQTT+HTTP config (AP and STA via `/setup`)
- **Preferences**: Persistent settings in NVS (namespaces: `wifi`, `mqtt`, `http`)
- **MQTT**: PubSubClient, connects per measurement, publishes JSON, disconnects
- **HTTP**: HTTPClient POST to configured webhook URL
- **NTP**: German timezone (CET/CEST) via `pool.ntp.org`, synced after WiFi connect

## BLE Scale Protocol (FitTrack / FT-DARA-WH01-GL)
- Scale advertises as `"FitTrack"` (device name `"SWAN"`)
- Chip: Dialog Semi DA14531
- Custom service: `0xFFB0`
  - `FFB1`: Write (commands to scale)
  - `FFB2`: Notify (weight data) — primary data source
  - `FFB3`: Write + Notify (command/response) — also subscribed for data
- **Packet format** (8 bytes on FFB2):
  - Bytes 0-1: Header `AC 02`
  - Bytes 2-3: Weight in 0.1 kg, big-endian (e.g., `04 03` = 1027 = 102.7 kg)
  - Bytes 4-5: Extra data
  - Byte 6: Type — `CE` = live, `CA` = stable/final, `CB` = post-measurement
  - Byte 7: Checksum (low byte of sum of bytes 2-6)
- Scale only activates BLE when someone steps on it
- Scale disconnects after measurement

## Important Design Decisions
- **Deferred forwarding**: BLE callbacks set `doForward = true`, main loop calls `forwardWeight()`. Network operations (MQTT/HTTP) in BLE callback context crash the ESP32.
- **No BLE reconnect cooldown**: Removed because it caused missed connections when stepping on the scale again quickly.
- **Non-blocking BLE scan**: `pBLEScan->start(5, callback, false)` — duration must be >0 (duration 0 never completes). Keeps web server responsive.
- **Separate settings forms**: WiFi, MQTT, and HTTP each have their own save endpoint. MQTT/HTTP save via AJAX with inline feedback (no page reload). WiFi save triggers a connection test and restart.
- **No live weight display**: Only the last final measurement is shown (with timestamp).

## Web Endpoints
- `GET /` — Weight page (STA) or config page (AP)
- `GET /setup` — Settings page (WiFi, MQTT, HTTP, API links)
- `POST /save/wifi` — Save WiFi credentials (tests connection, restarts on success)
- `POST /save/mqtt` — Save MQTT settings (AJAX, returns JSON)
- `POST /save/http` — Save HTTP webhook (AJAX, returns JSON)
- `GET /api/last-weight-data` — Last measurement JSON (`weight`, `time`)
- `GET /api/docs` — API documentation JSON
- `GET /api/scan` — WiFi network scan (used by config page)
- `GET /api/settings` — Current MQTT/HTTP settings (used by config page to prefill)

## Key Constants
All in `src/main.cpp` top section:
- `SCALE_NAME` — BLE advertised name to scan for (`"FitTrack"`)
- `AP_SSID` / `AP_PASSWORD` — Access point credentials
- `AP_TIMEOUT_MS` — AP auto-off (5 min / 300000ms)
- `WIFI_LOST_MS` — WiFi loss tolerance before AP fallback (60s / 60000ms)

## Hardware
- ESP32-WROOM-32 (ESP32-D0WD-V3) dev board
- Connected via USB serial (`/dev/cu.usbserial-0001`)
- Partition: `huge_app.csv` (3MB app, no OTA) — needed because BLE library is large

## Debugging
- **BLE raw packet logging**: `logRawData()` in `parseScaleData()` is commented out by default. Uncomment the call to see hex dumps of all BLE packets in the serial monitor — useful for reverse-engineering the scale protocol or diagnosing connection issues.

## Notes
- `include/config.h` is gitignored (legacy from early prototyping, no longer used)
- `volatile` variables are used for data shared between BLE callbacks and main loop
- WiFi+BLE share the ESP32 antenna — non-blocking scan helps stability
- Flash usage is ~57%, RAM ~19% — room for more features
