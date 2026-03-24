# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Bluetooth A2DP stereo audio transmitter running on a WEMOS LOLIN32 (original ESP32 with Bluetooth Classic). Reads stereo RCA analog input via ADC and streams to Bluetooth headsets/speakers. Wi-Fi AP web UI for device connection and control.

**Target hardware:** Original ESP32 only — not ESP32-S3 (no Bluetooth Classic on S3).

## Build & Upload

PlatformIO/Arduino project. Open in VS Code with PlatformIO extension.

- **Build:** `pio run`
- **Upload:** `pio run -t upload --upload-port COMx`
- **Serial monitor:** `python tools/monitor.py COMx 115200` → logs to `docs/logs/live.log`
- **Upload speed:** 921600 baud
- **Kill monitor before upload:** `powershell -Command "Get-Process python | Stop-Process -Force"`

Key dependency pinned in `platformio.ini`: `pschatzmann/ESP32-A2DP @ ^1.8.9`

## Architecture

All application logic in `src/main.cpp`. Library modifications in `.pio/libdeps/lolin32/ESP32-A2DP/src/BluetoothA2DPSource.cpp`.

### Hardware pins

| Pin | Function |
|-----|----------|
| GPIO34 | ADC1_CH6 — Left audio input |
| GPIO35 | ADC1_CH7 — Right audio input |
| GPIO5 | LED_BUILTIN — status LED (active HIGH) |

### Concurrent contexts

1. **Arduino main loop** (`loop()`) — HTTP server polling + LED state machine + BT retry logic
2. **A2DP library thread** — FreeRTOS tasks; audio callback `getDataFrames()` runs on Core 0
3. **ADC task** (`adcTask`) — pinned to Core 1; reads I2S ADC DMA, pushes stereo pairs to ring buffer

### Audio path

adc_continuous DMA (native CH6/CH7 alternating scan) → adcTask → ring buffer → phase-accumulator stereo resampler → A2DP Bluetooth stack

### Key callbacks

- `getDataFrames(Frame*, int32_t)` — stereo resampler with adaptive ratio, pops L/R pairs from ring
- `connectionStateChanged()` — manages WiFi/ADC lifecycle, BT bandwidth tricks (sleep_disable, DH3 packets)
- `audioStateChanged()` — fallback connection detection

### Boot flow (v1.0.12+)

- **With saved device:** Skip WiFi → BT stack init → MAC reconnect (no name scan) → ADC pre-fills ring → connected in ~5s
- **No saved device:** Start WiFi AP → wait for user → name scan → connect
- **Retry logic:** 3 MAC attempts → 2 name scans → give up → start WiFi AP as fallback

### Web UI

- `GET /` — status page with auto-refresh (JS polling every 3s)
- `GET /connect?name=<device>` — start BT connection
- `GET /forget` — clear saved device, reboot
- `GET /fullreset` — erase NVS (BT bonds + settings), reboot
- `GET /volume?level=N` — set input gain 0-100 (digital PCM scaling), persisted to NVS
- `GET /status` — JSON status endpoint for polling

### LED state indication (v1.0.13)

| State | Pattern |
|-------|---------|
| AP mode (waiting for user) | Breathing (smooth fade in/out) |
| Connecting to BT | Slow blink (~1Hz) |
| Connected + streaming | Solid ON |
| Disconnect / connection failed | Rapid blink (5Hz) for 3s, then slow blink (reconnecting) or breathing (AP fallback) |

## Library Modifications

`.pio/libdeps/lolin32/ESP32-A2DP/src/BluetoothA2DPSource.cpp`:
- `delay_ms(10000)` → `delay_ms(2000)` — faster BT stack settle
- Heartbeat timer 10s → 3s — faster reconnect retries
- AVRC volume handler fix — prevents reboot on headset volume change

## Documentation Rules

- **ALWAYS** update `docs/PROGRESS.md` with 🔲 TESTING BEFORE implementing changes
- **ALWAYS** update with ✅/❌ result AFTER testing
- Update `docs/ROADMAP.md` when completing or adding tasks
- Bump `kFwVersion` in main.cpp for every firmware change

## Usage

1. Join Wi-Fi **ESP32-Audio-Setup** / password **esp32audio**
2. Browse to **http://192.168.4.1**
3. Put headset in pairing mode, enter exact Bluetooth device name, click **Connect**
4. Audio streams automatically once connected
5. On subsequent boots, connects automatically via cached MAC (~5s)
