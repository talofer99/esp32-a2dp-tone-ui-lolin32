# ESP32 A2DP Stereo Audio Transmitter

Bluetooth A2DP audio transmitter running on a WEMOS LOLIN32 (original ESP32).
Reads **stereo RCA analog input** via ADC and streams to Bluetooth headsets/speakers.

Wi-Fi AP web UI for device connection and control — no app needed.

## Features

- Stereo ADC input: GPIO34 (Left) + GPIO35 (Right)
- A2DP source streaming to any Bluetooth Classic headset/speaker
- Phase-accumulator resampler with adaptive ratio
- eFuse ADC calibration for linearity correction
- Auto-reconnect to last paired device on boot
- Volume control via web UI (persisted to NVS)
- "Forget device" button to clear BT pairing
- Debug stats toggle (`#define DEBUG_STATS` in main.cpp)

## Hardware

| Item | Detail |
|------|--------|
| Board | WEMOS LOLIN32 (original ESP32 with Bluetooth Classic) |
| Left input | GPIO34 (ADC1_CH6) |
| Right input | GPIO35 (ADC1_CH7) |
| Bias circuit | 2×100K divider (3.3V→GPIO→GND) + 22µF coupling cap + 10K series resistor |
| Filter caps | 1nF ceramic low-pass + 10µF electrolytic bias bypass per channel |

**Note:** Must be original ESP32 — ESP32-S3 has no Bluetooth Classic.

See [docs/PROGRESS.md](docs/PROGRESS.md) for full circuit schematic.

## Build / Upload

PlatformIO project — open in VS Code with PlatformIO extension.

```
pio run -t upload --upload-port COMx
```

## Usage

1. Join Wi-Fi: **ESP32-Audio-Setup** / password **esp32audio**
2. Browse to **http://192.168.4.1**
3. Put headset in pairing mode
4. Enter exact Bluetooth device name → click **Connect**
5. Audio streams automatically once connected

## Architecture

- All application logic in `src/main.cpp`
- Two concurrent contexts: Arduino `loop()` (HTTP server) + A2DP FreeRTOS task (audio callback)
- Audio path: I2S ADC DMA → ring buffer → stereo resampler → A2DP Bluetooth stack
- Persistent storage: `Preferences` library (device name + volume)

## Serial Monitor

```
python tools/monitor.py COMx 115200
```

Output logged to `docs/logs/live.log`. Enable `#define DEBUG_STATS` in main.cpp for periodic ADC/A2DP stats.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md) for planned features.
