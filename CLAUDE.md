# OpenTrackFit - Development Guide

## Project Overview
ESP32 firmware (PlatformIO/Arduino) that reads weight data from a FitTrack BLE body composition scale and serves it via a web interface. Future: MQTT/HTTP forwarding.

## Build & Flash
```bash
pio run                # Build
pio run -t upload      # Flash to ESP32
pio device monitor     # Serial monitor (close before flashing!)
```

## Architecture
Single-file firmware (`src/main.cpp`) with these subsystems:
- **BLE**: Non-blocking scan for "FitTrack" scale, connects, subscribes to FFB2 notifications
- **WiFi**: STA mode with AP fallback (config portal)
- **WebServer**: Live weight page (STA) / WiFi config (AP)
- **Preferences**: Persistent WiFi credentials in NVS

## BLE Scale Protocol (FitTrack / FT-DARA-WH01-GL)
- Scale advertises as `"FitTrack"` (device name `"SWAN"`)
- Chip: Dialog Semi DA14531
- Custom service: `0xFFB0`
  - `FFB1`: Write (commands to scale)
  - `FFB2`: Notify (weight data) — this is what we use
  - `FFB3`: Write + Notify (command/response)
- **Packet format** (8 bytes on FFB2):
  - Bytes 0-1: Header `AC 02`
  - Bytes 2-3: Weight in 0.1 kg, big-endian (e.g., `04 03` = 1027 = 102.7 kg)
  - Bytes 4-5: Extra data
  - Byte 6: Type — `CE` = live, `CA` = stable/final, `CB` = post-measurement
  - Byte 7: Checksum (low byte of sum of bytes 2-6)
- Scale only activates BLE when someone steps on it
- Scale disconnects after measurement (or ESP disconnects after 10s idle timeout)

## WiFi Behavior
- On boot: tries saved credentials → success = STA mode, fail = AP mode
- AP mode: SSID `OpenTrackFit`, pass `12345678`, auto-off after 5 min
- STA mode: if WiFi lost for 60s → falls back to AP mode
- Config portal tests WiFi before saving (shows success/failure)

## Key Constants
All in `src/main.cpp` top section:
- `SCALE_NAME` — BLE advertised name to scan for
- `AP_SSID` / `AP_PASSWORD` — Access point credentials
- `IDLE_TIMEOUT_MS` — BLE disconnect after no data (10s)
- `AP_TIMEOUT_MS` — AP auto-off (5 min)
- `WIFI_LOST_MS` — WiFi loss tolerance before AP fallback (60s)

## Hardware
- ESP32-WROOM-32 (ESP32-D0WD-V3) dev board
- Connected via USB serial (`/dev/cu.usbserial-0001`)
- Partition: `huge_app.csv` (3MB app, no OTA) — needed because BLE library is large

## Notes
- `include/config.h` is gitignored (was used for early prototyping, no longer needed)
- PubSubClient is in `lib_deps` but MQTT is not yet implemented
- BLE scan is non-blocking to keep the web server responsive
- `volatile` variables are used for data shared between BLE callbacks and main loop
