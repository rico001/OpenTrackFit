# OpenTrackFit

ESP32-based bridge that reads weight data from a Bluetooth (BLE) body composition scale and makes it available via a local web interface.

## Supported Hardware

- **Microcontroller**: ESP32-WROOM-32 (any ESP32 dev board)
- **Scale**: FitTrack Body Composition Smart Scale (Model: FT-DARA-WH01-GL)
  - Other scales advertising as "FitTrack" with BLE service `0xFFB0` should also work

## Features

- Automatic BLE connection to scale when someone steps on it
- Live weight display on a web page in your local network
- WiFi configuration via captive portal (no hardcoded credentials)
- Auto-reconnect after scale powers off
- mDNS support (`http://opentrackfit.local`)

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE plugin)
- ESP32 dev board connected via USB

### Build & Flash

```bash
pio run -t upload
```

### Initial WiFi Setup

1. On first boot, the ESP32 creates a WiFi access point:
   - **SSID**: `OpenTrackFit`
   - **Password**: `12345678`
2. Connect to the AP and open `http://192.168.4.1`
3. Select your home WiFi network and enter the password
4. The ESP32 tests the connection and shows success or failure
5. On success, the ESP32 restarts and joins your network

### Usage

1. Open `http://opentrackfit.local` (or the IP shown in the serial log)
2. Step on the scale — the live weight appears on the page
3. The final weight is displayed after the measurement stabilizes

## Serial Monitor

```bash
pio device monitor
```

Example output:
```
=== OpenTrackFit ===
Mode:  LAN (Station)
IP:    192.168.1.42
mDNS:  http://opentrackfit.local
-----------------------------
FitTrack found!
Connecting to 03:b3:ec:c1:cf:24...
Connected. Waiting for measurement...

  Measuring: 102.6 kg
  Measuring: 102.7 kg
>>> FINAL WEIGHT: 102.7 kg <<<
>> Scale disconnected. Rescanning...
```

## WiFi Behavior

| Situation | Behavior |
|-----------|----------|
| First boot (no saved WiFi) | Starts AP mode for configuration |
| Saved WiFi available | Connects to WiFi (STA mode) |
| WiFi lost for >60 seconds | Falls back to AP mode for 5 minutes |
| AP mode timeout (5 min) | Retries saved WiFi credentials |

## Roadmap

- [ ] MQTT publishing of weight data
- [ ] HTTP POST callback to external services
- [ ] Weight history / trend tracking
- [ ] Multi-user support
- [ ] Body composition data parsing (fat %, muscle mass, etc.)

## License

MIT
