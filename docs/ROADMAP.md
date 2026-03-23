# ESP32 A2DP Audio — Project Roadmap

## What it does today (v1.0.11)

- ESP32 (LOLIN32) boots as a Wi-Fi access point (`ESP32-Audio-Setup` / `esp32audio`)
- User enters exact Bluetooth device name → ESP32 connects as A2DP source
- Streams **stereo** analog audio from GPIO34 (L) + GPIO35 (R) to the headset over Bluetooth
- Phase-accumulator resampler with adaptive ratio (handles both DH3 and DH5 BT modes)
- eFuse ADC calibration for linearity correction
- Volume slider in the web UI (0–100), persisted to NVS
- Last connected device saved — auto-reconnects on reboot
- "Forget device" button clears NVS and reboots for fresh pairing
- `esp_bt_sleep_disable()` ensures full BT bandwidth (346 calls/sec)
- Debug stats behind `#define DEBUG_STATS` toggle

## Hardware

| Item | Detail |
|------|--------|
| Board | WEMOS LOLIN32 (original ESP32 with Bluetooth Classic) |
| Audio input | GPIO34 (L) + GPIO35 (R) — stereo, line-level via bias circuit |
| Bias circuit | 2×100K divider + 22µF electrolytic coupling cap + 10K series + 1nF LP + 10µF bias bypass |
| Power decoupling | 100nF ceramic + 10µF electrolytic on 3.3V rail near ESP32 |

**Note:** Must be original ESP32 — ESP32-S3 has no Bluetooth Classic, A2DP source won't work.

---

## Backlog / TODO

### Priority — Next up

- [ ] **LED state indication** — use onboard or external LED to show current state (booting, waiting for BT, connecting, streaming, error)
- [ ] **Faster BT connection** — investigate reducing discovery timeout, caching device address, or skipping name resolution for known devices
- [ ] **Reduce connection-to-audio delay** — minimize time between BT connected and first audio frame (pre-fill ring buffer, faster SYSCON apply)
- [ ] **Web UI keep-alive** — add JS polling or WebSocket so the status page stays updated without manual refresh; handle WiFi→BT transition gracefully
- [ ] **Physical clear button** — hardware button (GPIO) to clear NVS and reset BT pairing without needing WiFi/web UI access

### UX improvements

- [ ] **Bluetooth device scan** — use `a2dp_source.discover_async()` to scan for nearby Classic BT devices and show a tap-to-connect list
- [ ] **Auto-start audio** — start streaming automatically when BT connects (skip manual button)
- [ ] **Status auto-refresh** — small JS polling so the status card updates without manual reload

### Audio quality

- [ ] **Evaluate new IDF I2S/ADC API** — current code uses legacy `driver/i2s.h` (deprecated in IDF 5.x); migrate to `esp_adc/adc_continuous.h` if quality issues persist
- [ ] **Input gain stage** — optional op-amp (e.g. MCP6002) if line-level signal is too weak

### Hardware / phase 2

- [ ] **Enclosure** — finalise physical connector layout and enclosure design
- [ ] **Power supply** — USB vs 5V wall adapter; decouple analog supply from digital to reduce hum

### Housekeeping

- [ ] **Split main.cpp into modules** — separate into logical files: BT/A2DP, ADC/audio, web server/AP, core logic. Currently everything is in one 600+ line file.
- [ ] Silence legacy I2S/ADC deprecation warnings in build output
- [ ] Add OTA update support (requires switching from `huge_app` to a dual-OTA partition table)

---

## Architecture notes

- All app logic in `src/main.cpp`
- Two concurrent contexts: Arduino `loop()` (HTTP) + A2DP FreeRTOS task (audio callback)
- Audio path: I2S ADC DMA → ring buffer → phase-accumulator stereo resampler → A2DP Bluetooth stack
- Persistent storage: `Preferences` library, namespace `"audio"`, keys `"device"` (String) and `"volume"` (int)
- Partition scheme: `huge_app.csv` (3 MB app, no OTA) — set in `platformio.ini`
- Key BT fix: `esp_bt_sleep_disable()` — without this, some ESP32 chips negotiate DH5 at half bandwidth
