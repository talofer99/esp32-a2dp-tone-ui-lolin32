# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Phase 1 proof-of-concept Bluetooth A2DP audio transmitter running on a WEMOS LOLIN32 (original ESP32 with Bluetooth Classic). Creates a Wi-Fi AP and serves a web UI to connect to Bluetooth headsets and play a sine-wave test tone.

**Target hardware:** Original ESP32 only — not ESP32-S3 (no Bluetooth Classic on S3).

## Build & Upload

This is a PlatformIO/Arduino project. Open in VS Code with the PlatformIO extension installed.

- **Build:** PlatformIO sidebar → `lolin32` environment → Build
- **Upload:** PlatformIO sidebar → Upload (or `pio run -t upload`)
- **Serial monitor:** 115200 baud (`pio device monitor`)
- **Upload speed:** 921600 baud

Key dependency pinned in [platformio.ini](platformio.ini): `pschatzmann/ESP32-A2DP @ ^1.8.9`

## Architecture

All application logic lives in a single file: [src/main.cpp](src/main.cpp)

### Two concurrent contexts

1. **Arduino main loop** (`loop()`) — polls `server.handleClient()` every 10ms to serve HTTP requests.
2. **A2DP library thread** — ESP32-A2DP runs its own FreeRTOS tasks; communicates back via two registered callbacks.

### Callbacks (cross-thread communication)

- `getDataFrames(Frame*, int32_t)` — called by the A2DP library requesting audio samples. Generates sine-wave frames via phase accumulation if `toneEnabled && btConnected`.
- `connectionStateChanged(esp_a2d_connection_state_t, void*)` — updates `btConnected` and `lastBtState` (both `volatile`/global).

### Web UI flow

- `GET /` — renders inline HTML status page with current state.
- `GET /connect?name=<device>` — sets `targetDeviceName`, calls `a2dp_source.start()` (one-time; requires reboot to change device).
- `GET /tone/on` and `/tone/off` — toggle `toneEnabled`.
- `htmlEscape()` sanitizes user input before embedding in HTML output.

### Tone generation

Phase-accumulation sine wave at 523.25 Hz (C5), 44100 Hz sample rate, amplitude 9000, output as stereo `int16_t` frames (both channels identical).

## Usage (after flashing)

1. Join Wi-Fi `ESP32-Audio-Setup` / password `esp32audio`
2. Browse to `http://192.168.4.1`
3. Put headset in pairing mode, enter exact Bluetooth device name, click **Connect**
4. Click **Play tone** once connected

## Known Limitations / Planned Work

- No Bluetooth device scan — must know exact device name
- No reconnect manager — reboot to change target device
- Analog (RCA) input not yet implemented — planned for next iteration
