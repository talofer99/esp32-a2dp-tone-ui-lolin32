# ESP32 A2DP Audio — Project Roadmap

## What it does today

- ESP32 (LOLIN32) boots as a Wi-Fi access point (`ESP32-Audio-Setup` / `esp32audio`)
- Captive portal: connecting to the AP pops up the control page automatically
- User enters exact Bluetooth device name → ESP32 connects as A2DP source
- Streams mono analog audio from GPIO34 (ADC1_CH6) to the headset over Bluetooth
- Volume slider in the web UI (0–100), applied in the audio callback
- Last connected device + volume saved to NVS — auto-reconnects on reboot
- "Forget device" button: TBD (see backlog below)

## Hardware

| Item | Detail |
|------|--------|
| Board | WEMOS LOLIN32 (original ESP32 with Bluetooth Classic) |
| Audio input | GPIO34 — mono, line-level via bias circuit (2×100kΩ divider + 10kΩ series + 10µF cap) |
| Target | Bluetooth Classic A2DP headphones/speakers |

**Note:** Must be original ESP32 — ESP32-S3 has no Bluetooth Classic, A2DP source won't work on it.

## End goal

Replace the analog-in circuit with RCA jacks from a hi-fi / TV, so the ESP32 sits
in-line as a wireless adapter: **RCA analog in → Bluetooth A2DP out**.

---

## Backlog / TODO

### UX improvements
- [ ] **Forget saved device** — button in the UI to clear NVS (`prefs.remove("device")`) and reset BT source so a new device can be paired without rebooting
- [ ] **Bluetooth device scan** — use `a2dp_source.discover_async()` to scan for nearby Classic BT devices and show a tap-to-connect list, so user never needs to type an exact name
- [ ] **Auto-start audio** — option to start streaming automatically when BT connects (skip the "Start audio" button)
- [ ] **Status auto-refresh** — small JS polling so the status card updates without a manual page reload

### Audio quality
- [ ] **Stereo input** — add second channel on GPIO35 (ADC1_CH7), same bias circuit, output true L/R
- [ ] **ADC calibration** — apply ESP32 ADC non-linearity correction (`esp_adc_cal`) for cleaner signal
- [ ] **Evaluate new IDF I2S/ADC API** — current code uses the legacy `driver/i2s.h` (deprecated in IDF 5.x); migrate to `esp_adc/adc_continuous.h` + timer-driven approach if quality issues persist

### Hardware / phase 2
- [ ] **RCA jack wiring** — finalise physical connector and enclosure
- [ ] **Input gain stage** — optional op-amp (e.g. MCP6002) if line-level signal is too weak after ADC scaling
- [ ] **Power supply** — USB vs 5V wall adapter; decouple analog supply from digital to reduce hum

### Housekeeping
- [ ] Silence legacy I2S/ADC deprecation warnings in build output
- [ ] Add OTA update support (requires switching from `huge_app` to a dual-OTA partition table)

---

## Architecture notes

- All app logic in `src/main.cpp`
- Two concurrent contexts: Arduino `loop()` (HTTP) + A2DP FreeRTOS task (audio callback)
- Audio path: I2S ADC DMA → `getDataFrames()` callback → A2DP Bluetooth stack
- Persistent storage: `Preferences` library, namespace `"audio"`, keys `"device"` (String) and `"volume"` (int)
- Partition scheme: `huge_app.csv` (3 MB app, no OTA) — set in `platformio.ini`
