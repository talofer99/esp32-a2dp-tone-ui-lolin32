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

- [x] **LED state indication** — onboard LED (GPIO5): breathing=AP mode, slow blink=connecting, solid=streaming, rapid blink=disconnect/failure alert (v1.0.13)
- [x] **Faster BT connection** — library delay 10s→2s, heartbeat 10s→3s, MAC-based reconnect skips name scan (v1.0.12)
- [x] **Reduce connection-to-audio delay** — ADC pre-fills ring buffer during BT connect, no silence gap on first audio frame (v1.0.12)
- [ ] **Keep WiFi AP alive during BT streaming** — currently `esp_wifi_stop()` kills the web UI on BT connect; investigate running both concurrently (heap permitting) so user can adjust volume/disconnect from the UI
- [ ] **Physical clear button** — hardware button (GPIO) to clear NVS and reset BT pairing without needing WiFi/web UI access

### UX improvements

- [x] **Save last volume level** — already persisted to NVS on change + restored on boot (was already implemented)
- [x] **Better volume control** — removed hardcoded `set_volume(30)`, renamed slider to "Input Gain" to clarify it's digital gain not headset volume. Use headset buttons for actual volume (v1.0.13)
- [ ] **Bluetooth device scan** — use `a2dp_source.discover_async()` to scan for nearby Classic BT devices and show a tap-to-connect list
- [ ] **Re-evaluate web UI content** — review what info is shown, layout, and usefulness; simplify or reorganize as needed
- [x] **Auto-start audio** — streaming starts automatically on BT connect (`toneEnabled = true` in connectionStateChanged)
- [x] **Status auto-refresh** — JS polling every 3s via `setInterval(refresh, 3000)` + `/status` endpoint

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
