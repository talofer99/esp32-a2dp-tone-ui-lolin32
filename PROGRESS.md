# ESP32 A2DP Audio — Debug Progress Log

Keep this file updated after every change. When starting a new session, read this file first.

---

## Hardware
- WEMOS LOLIN32 (original ESP32, Bluetooth Classic)
- GPIO34 → ADC input (RCA analog in)
- Sends audio to BT headset via A2DP source

## Current Firmware State

### Key Constants (`src/main.cpp`)
| Constant | Value | Why |
|---|---|---|
| `kI2SConfigRate` | 53878 Hz | Hardware delivers ~44,160 Hz actual (82% efficiency) |
| `kResampleRatio` | 1.0299 = 44160/42880 | Matches fill rate to drain rate at 335 BT calls/sec |
| `RING_SIZE` | 4096 | ~93ms buffer |
| `kMidpointMv` | 1650 | DC bias for 2×100kΩ divider on 3.3V |

### Library Modifications (`.pio/libdeps/lolin32/ESP32-A2DP/src/BluetoothA2DPSource.cpp`)
| Location | Change | Reason |
|---|---|---|
| STARTING→STARTED transition (~line 836) | `xTimerChangePeriod(s_tmr, pdMS_TO_TICKS(60000), 0)` | Attempted slow heartbeat — **see DISPROVEN ASSUMPTIONS below** |
| STOPPING→IDLE transition (~line 859) | `xTimerChangePeriod(s_tmr, pdMS_TO_TICKS(10000), 0)` | Restore 10s heartbeat for reconnect |
| AVRC volume handler | Fixed feedback loop crash | Prevents reboot on headset volume change |
| CONNECTING→CONNECTED (~line 684) | `esp_bt_gap_set_acl_pkt_types(...)` | **Force DH3 packet type — the actual fix for 167/sec** |

---

## Root Cause — Definitively Identified

**DH5 vs DH3 BT ACL packet type negotiation:**

| Packet type | Slots | Period | Max rate | Callback rate | Result |
|---|---|---|---|---|---|
| DH3 (master) + DH1 (slave ack) | 3+1=4 | 2.5ms | 400/sec | ~335/sec | ✅ Continuous audio |
| DH5 (master) + DH5 (slave ack) | 5+5=10 | 6.25ms | 160/sec | ~167/sec | ❌ Choppy |

With DH5: 167/sec × 128 samples = 21,376/sec but headset plays at 44,100 Hz → runs out every 0.485s → 1s on/1s off pattern.

The headset chooses DH5 on "good" RF links (more efficient). The ESP32 must explicitly restrict to DH3 via `esp_bt_gap_set_acl_pkt_types()`.

---

## ASSUMPTIONS AND STATUS (Track every assumption here!)

| # | Assumption | Status | Evidence |
|---|---|---|---|
| A1 | 60s heartbeat causes 335/sec BT rate | ❌ DISPROVEN | Log shows heartbeat still fires every 10s — xTimerChangePeriod(60000, 0) silently fails (blockTime=0 drops command if FreeRTOS timer queue full) |
| A2 | Full NVS erase + fresh pair restores 335/sec | ❌ DISPROVEN | Still 167/sec after NVS erase and fresh pair |
| A3 | Heartbeat timer irrelevant to 167/sec issue | ✅ CONFIRMED | STARTED state handler just logs; timer period doesn't affect BT packet negotiation |
| A4 | 167/sec is caused by DH5 packet type negotiation | ✅ CONFIRMED | Math: 5+5 slot period = 160 Hz ≈ 167/sec observed; 335/sec matches 3+1 slot DH3+DH1 |
| A5 | `esp_bt_gap_set_acl_pkt_types()` at connection time forces DH3 | ❌ DISPROVEN | Calling at CONNECTING→CONNECTED made it WORSE: dropped from 167/sec to 82/sec. Headset likely responded to packet type change during AVDTP negotiation by entering slow retransmission mode |
| A6 | Same call AFTER media starts (STARTING→STARTED) won't disturb AVDTP and will force DH3 | ❌ DISPROVEN | Still 82/sec. This headset (SD01) consistently responds to esp_bt_gap_set_acl_pkt_types() by slowing down to 82/sec regardless of timing. DO NOT USE this API with SD01. |
| A7 | Disconnect + auto-reconnect gives ~50% chance of DH3 on new connection | 🔲 TESTING | Auto-connect on boot got DH3 once (346/sec). Next boot got DH5 again. Trying auto-disconnect loop when DH5 detected for 10s. |
| A8 | I2S RIGHT_LEFT stereo mode alternates CH6/CH7 samples with channel ID in upper nibble | ❌ DISPROVEN | ringR stayed 0 — all samples had channel ID 6. Also: stereo I2S caused crash loop when BT stack initialized. REVERTED to mono. |
| A9 | SYSCON patt_tab write after i2s_set_adc_mode makes ADC1 alternate CH6/CH7 | ✅ CONFIRMED | SYSCON written from adcTask (Core 1) after adcEnabled=true: L:22080/sec R:22080/sec stereo:yes ✅ |
| A10 | I2S ONLY_LEFT + i2s_driver_install in setup() (before BT) is safe | ✅ CONFIRMED | Working mono firmware proves this. Crash in step 14 was caused by RIGHT_LEFT format, not by setup() timing. |
| A11 | Re-applying SYSCON after i2s_adc_enable() prevents reset | ✅ CONFIRMED | SYSCON written in adcTask after adcEnabled=true (which fires after i2s_adc_enable) → both confirmed working |
| A12 | Accessing ringBufR (via ringRAvail/ringRPop) from getDataFrames (Core 0 BT task) is safe | ✅ PARTIAL | ringRAvail() alone: ✅ GDF called. ringRPop() + discard: ✅ GDF called. ringRPop() + assign to variable + write to frame[i].channel2: ❌ GDF never called. Root cause unknown. |
| A13 | Writing a ringR-derived value to frame[i].channel2 silently kills the BT data callback | ✅ CONFIRMED | Bisect: pop+discard works, pop+use in channel2 = GDF never invoked. WORKAROUND: use interleaved single ring (no ringBufR in getDataFrames). |
| A14 | Direct pop (no resampler) works for stereo — drain≈fill naturally | ❌ DISPROVEN | Drain = 335 calls/sec × 108 frames = 36,180 pairs/sec >> fill = 22,080 pairs/sec → ring empties → silence bursts → constant electric noise + high pitch. Resampler is mandatory. |
| A15 | Hard noise gate (±40mV threshold) reduces hiss without audible artifacts | ❌ DISPROVEN | Gate creates step discontinuity every time signal crosses threshold → clicking/crackling on every transient edge, worse than original hiss. | Drain = 335 calls/sec × 108 frames = 36,180 pairs/sec >> fill = 22,080 pairs/sec → ring empties → silence bursts → constant electric noise + high pitch. Resampler is mandatory. |

---

## The Core Problem Being Solved

**BT data callback rate determines audio quality:**
- ✅ **335/sec × 128 samples = 42,880/sec** → drain ≈ fill (44,160) → continuous audio
- ❌ **167/sec × 128 samples = 21,376/sec** → only 48.5% of needed data → **1s music / 1s silence**

---

## Session History

### Previous Session (ended well — 7-8s gaps, otherwise OK)
- I2S fill: 44,160/sec ✅
- BT callbacks: **335/sec** ✅ (continuous audio)
- Known issue: periodic 7-8s gap (cause not yet identified)
- Library had: 60s heartbeat mod (which we now know silently fails!) + AVRC fix
- **The 335/sec was NOT caused by the heartbeat — it was because the headset happened to negotiate DH3 on that first pairing**

### This Session — What We Changed and Results

| # | Change | Result |
|---|---|---|
| 1 | Reverted 60s heartbeat → 10s | BT rate dropped to **167/sec** → 1s on/1s off ❌ |
| 2 | Added btStreamTask + `esp_a2d_source_data_ready()` | Compile error — function doesn't exist in this ESP-IDF |
| 3 | Removed btStreamTask | Compiled OK, still 167/sec |
| 4 | Restored 60s heartbeat | Still 167/sec — confirmed heartbeat is NOT the cause |
| 5 | Added `/fullreset` endpoint (wipes all NVS incl. Bluedroid) | Still 167/sec after fresh pair |
| 6 | Power cycle + fresh pair | Still 167/sec — headset defaulted to DH5 |
| 7 | `esp_bt_gap_set_acl_pkt_types()` at CONNECTING→CONNECTED | ❌ Made WORSE: 82/sec. Reverted. |
| 8 | Reverted to step 6 baseline | Back to 167/sec |
| 9 | `esp_bt_gap_set_acl_pkt_types()` AFTER media starts | ❌ Still 82/sec. Fully abandoned. |
| 10 | Reverted again + fixed xTimerChangePeriod blockTime → portMAX_DELAY | Back to 167/sec |
| 11 | Added auto-connect on boot | ✅ Device reconnects automatically without web UI |
| 12 | Adaptive kResampleRatio (was fixed 1.0299) — adjusts every 2s to keep ring near 1500 | ✅ Working — ratio converged 1.0→0.9960, ring 3700-3970, metallic sound gone. "Very good." |
| 13 | DH5 auto-reconnect: after 10s at 167/sec, disconnect() + let library auto-reconnect | 🔲 TESTING — Expected: ~50% chance each attempt gets DH3 (346/sec). Max 5 attempts. |
| 14 | Stereo: GPIO35 (ADC1_CH7) right channel + I2S RIGHT_LEFT mode | ❌ REVERTED — Crash loop on BT init. ringR=0 (stereo I2S doesn't alternate channels as expected). Reverted to mono. |
| 15 | Stereo v2: SYSCON pattern table, I2S in setup(), re-apply SYSCON after i2s_adc_enable | ✅ ADC: L:22080/sec R:22080/sec stereo:yes. BUT getDataFrames never called when channel2 uses ringR data — see A12/A13. |
| 16 | Bisect getDataFrames: added rsPhaseR vars (unused) | ✅ GDF still called |
| 17 | Bisect: added (void)ringRAvail() | ✅ GDF still called |
| 18 | Bisect: added ringRPop() + discard result | ✅ GDF still called |
| 19 | Bisect: added full rsPhaseR while-loop + sR computation + channel2=sR | ❌ GDF NOT called |
| 20 | Bisect: direct pop + assign to channel2 (no while loop) | ❌ GDF NOT called |
| 21 | Single interleaved ring: adcTask pushes CH6+CH7 to ringBuf (no ringBufR used in getDataFrames). Pop 2 per frame (L=channel1, R=channel2). No resampler for R, no rsPhaseR. Key: only ringBuf used in BT task (mono baseline used ringBuf for channel1 and it worked). | ✅ GDF called! calls:346/sec (DH3), ring:769 stable. BUT sound horrible — ring alignment wrong: CH6-only samples prefill ring before SYSCON, so getDataFrames pops (CH6,CH6) pairs instead of (CH6,CH7). |
| 22 | Fix alignment: (1) flush ring when SYSCON applied (ringWrite=0, ringRead=0); (2) push only COMPLETE pairs (hold CH6 in pendingL, push pendingL+CH7 together when CH7 arrives) — guarantees ring always has clean L,R,L,R alignment. | ❌ WORSE — constant electric noise, barely music, high pitch. Root cause: no resampler → drain >> fill (see A14). |
| 23 | Add stereo resampler in getDataFrames: phase accumulator pops ring pairs (L,R) when phase≥1.0, linear-interpolates both channels, adaptive ratio ~0.515 (fill_pairs/drain_pairs). Fixes A14. | ✅ CONFIRMED — drain:44288/sec calls:346/sec ratio:0.516 ring:stable |
| 24 | Add firmware version string (v1.0.0). Print on startup + every 2s in ADC stats as [v1.0.0]. | ✅ CONFIRMED — log shows [v1.0.0][ADC] L:22080/sec R:22080/sec stereo:yes |
| 25 | Fix speed variations: shrink adaptive ratio step 0.002→0.0001, widen deadband ±256→±512. | ✅ CONFIRMED v1.0.1 — speed wobble gone. ratio stable at 0.5151. |
| 26 | Fix crackling: apply eFuse ADC calibration (`esp_adc_cal_raw_to_voltage`). | ✅ CONFIRMED v1.0.2 — part of overall improvement. |
| 27 | Noise gate ±40mV — REVERTED. | ❌ REVERTED — hard gate caused step-discontinuity crackling on every transient (A15). |
| 28 | Remove noise gate + reduce amplitude 19→12 for clipping headroom. | ✅ CONFIRMED v1.0.2 — "much better". Edge crackling gone. Bass great. Mid OK. High pitch not best. Remaining: background noise between tracks. |
| 29 | Soft noise gate (expander): ramp from 0 at |adc|<25mV to full pass at |adc|>80mV. | ❌ REVERTED — affects music quality. Between-track noise confirmed as inherent hardware ADC floor, not fixable in software without degrading signal. Accepted as hardware limitation. |
| 30 | Revert soft gate. Back to clean pass-through (v1.0.2 audio path). | ✅ CONFIRMED v1.0.4 — running cleanly. |

---

## Stereo Plan (Step 15)

Root cause of step 14 failures:
1. I2S driver installed in `setup()` before BT → conflicts with BT stack init → crash loop
2. `I2S_CHANNEL_FMT_RIGHT_LEFT` with `i2s_set_adc_mode(CH6)` only scanned CH6 — no automatic CH7

New approach fixes both:

| Stage | Change | Why |
|-------|--------|-----|
| 1 | Move `i2s_driver_install` + `i2s_set_adc_mode` from `setupADC()` to `enableADC()` | Installs driver AFTER BT connects — eliminates crash |
| 2 | After `i2s_set_adc_mode`, write SYSCON pattern table: `saradc_sar1_patt_len=1`, `saradc_sar1_patt_tab[0]=(0x6F<<24)|(0x7F<<16)|…` | Forces ADC1 to alternate-scan CH6+CH7 in hardware |
| 3 | Move `xTaskCreatePinnedToCore(adcTask...)` to `enableADC()`, guarded by `static bool` | Start task after I2S driver is ready |
| 4 | Keep `I2S_CHANNEL_FMT_ONLY_LEFT` + `tmp[256]` — stream is mono-width | All samples arrive in one stream; demux by channel ID bits[15:12] |
| 5 | Add `ringBufR[RING_SIZE]` + `ringRAvail/ringRPush/ringRPop`; demux CH6→ringL / CH7→ringR in adcTask | Per-channel ring buffers |
| 6 | Add `rsPrevR/rsCurrR/rsPhaseR` in `getDataFrames`; `channel1=outL`, `channel2=outR` | True stereo to BT headset |
| 7 | Adaptive ratio formula: `44160.0f / (callCount*128)`, clamp 0.40–0.60 | Per-channel fill≈22080/sec → ratio≈0.498 at 346/sec |

**A9 (assumption):** SYSCON pattern table with `patt_len=1` makes ADC1 alternate CH6/CH7. Both will appear in the I2S stream with correct channel IDs in bits[15:12].

Channel map: CH6 = ADC1_CHANNEL_6 = GPIO34 (left), CH7 = ADC1_CHANNEL_7 = GPIO35 (right)
Pattern entries: CH6 = `0x6F` = (6<<4)|(3<<2)|3 (atten DB_11, 12-bit). CH7 = `0x7F` = (7<<4)|(3<<2)|3.

---

## Known Hardware Limitations

| Issue | Root cause | Fixable? |
|---|---|---|
| High-pitch rolloff | ADC alternates CH6/CH7 → 22080 Hz per channel → Nyquist 11kHz; linear interpolation adds -3.9dB at 11kHz | No (hardware) |
| Background noise floor | ESP32 ADC inherent noise ~30–50mV (not a hi-fi chip) | Partial (soft gate) |
| BT sleep halves throughput | New ESP32 chips default to BT sleep → 167/sec instead of 346/sec | Yes: `esp_bt_sleep_disable()` |

## Hardware Circuit (per channel — mirror for L/R)

**Coupling cap:** 22µF electrolytic (NOT 22nF — 22nF cuts bass at 723 Hz, confirmed horrible).
- 22µF + 10K → fc ≈ 0.7 Hz (passes all audio including sub-bass)

```
3.3V rail:
  3.3V → 100K resistor → GPIOxx node
  GPIOxx node → 100K resistor → GND

Signal path:
  RCA tip → (+) 22µF electrolytic (−) → 10K resistor → GPIOxx node

Filter caps at GPIOxx node:
  GPIOxx node → 1nF ceramic → GND           (low-pass, fc ≈ 16kHz with 10K)
  GPIOxx node → 10µF electrolytic (+→node, −→GND)  (bias bypass)

RCA shield → GND
```

**Left = GPIO34, Right = GPIO35**

**Shared power decoupling (one set, near ESP32):**
  3.3V → 100nF ceramic → GND
  3.3V → 10µF electrolytic (+→3.3V, −→GND) → GND

---

## Step 31 — Disable DH5 reconnect loop (v1.0.5) ✅

**Problem:** New ESP32 (COM6) + SD01 headset negotiates DH5 (167 calls/sec). DH5 reconnect logic disconnects every 10s, creating infinite connect/disconnect loop — no audio plays.
**Root cause:** This headset only supports DH5. DH3 packet type was already tried in library and rejected (82/sec — worse).
**Fix:** Disable DH5 reconnect logic.
**Result:** Connection stays up ✅, but audio scrambled — ratio stuck at 0.517, ring full at 4095.

## Step 32 — Raise ratio clamp for DH5 (v1.0.6) ❌ PARTIAL

**Problem:** Resampler ratio clamped at 0.60 max. DH5 needs ratio ~1.03 (fill 22,080 / drain 21,440). Ring overflows → scrambled audio.
**Fix:** Raise ratio clamp from 0.60 to 1.20.
**Result:** Clamp raised, but adaptive step 0.0001 per 2s too slow — would take hours to reach 1.03 from 0.515. Ring stayed full at 4095.

## Step 33 — Two-speed adaptive ratio (v1.0.7) ❌ TOO SLOW

**Problem:** Adaptive ratio step 0.0001 per 2s is too slow when DH5 needs a 2x ratio change (0.515→1.03).
**Fix:** Two-speed adaptation: 1% jump when far off, 0.1% when moderate.
**Result:** Still takes ~2 min to converge. Audio choppy during ramp-up. Not good enough.

## Step 34 — Direct ratio calculation from drain rate (v1.0.8) ✅ RATIO WORKS, ❌ AUDIO BAD

**Problem:** Adaptive hunting too slow. Need instant ratio for any BT rate.
**Fix:** Compute ratio = 22080 / drainPerSec directly. Blend 80/20.
**Result:** Ratio jumps to 1.03 in 4 seconds ✅. But audio is "bursts of sped-up music with silence."
**Root cause:** DH5 at 167/sec × 128 frames = 21,440 frames/sec. Headset expects 44,100/sec → buffer underruns. Ratio can't fix this — the BT transport is the bottleneck.

## Step 35 — Force DH3 packet type from main.cpp (v1.0.9) ❌ NO EFFECT

**Problem:** DH5 provides half the needed BT bandwidth (21,440 vs 44,100 frames/sec).
**Fix:** esp_bt_gap_set_acl_pkt_types() returned ESP_OK but headset stayed at 167/sec.
**Also tried:** NVS erase + fresh pairing → still 167/sec. This ESP32 chip negotiates DH5 with SD01.

## Step 36 — BT bandwidth tricks (v1.0.10) ✅ FIXED!

**Problem:** Still stuck at 167 calls/sec (DH5). Need to maximize BT radio utilization.
**Fix — the key trick was `esp_bt_sleep_disable()`:**
1. ✅ `esp_bt_sleep_disable()` before AND after connection — **THIS was the fix**
2. `esp_bt_gap_set_scan_mode(NON_CONNECTABLE, NON_DISCOVERABLE)` — free scan slots
3. DH3 packet type request (kept from v1.0.9)
**Result:** 346 calls/sec ✅, drain 44,288/sec ✅, ratio ~0.50 ✅. Identical to old ESP32!
**Root cause:** BT controller was entering sleep/power-save between packet transmissions, halving the available radio time. `esp_bt_sleep_disable()` keeps the radio always active.
**Audio quality:** "Sounds good!" — soldered connections also improved noise.
**Version:** v1.0.10

---

## Current Status

**Firmware:** v1.0.10 (flashed and confirmed running on COM6)
**BT:** DH3 — 346 calls/sec ✅ (fixed via esp_bt_sleep_disable)
**ADC:** L:22080/sec R:22080/sec stereo:yes (SYSCON CH6+CH7 alternating) ✅
**Resampler:** ratio 0.5151 stable, ring 769–1149 ✅
**Audio quality:** "Much better" — music clear, singer intelligible, no gaps, no breaks, no speed wobble, no edge crackling.
**Remaining issue:** Low-level background noise audible between tracks (ESP32 ADC noise floor ~30–50mV, no gate applied).

### What the numbers look like (167/sec — BAD):
```
[A2DP] drain: 22080/sec  calls: 167/sec  avg: 131  ring: 3963
[ADC]  fill: 44160/sec   reads: 172/sec  avg bytes/read: 512  ring: 4095
```

### What the numbers look like (335/sec — GOOD):
```
[A2DP] drain: 44160/sec  calls: 335/sec  avg: 131  ring: ~2000
[ADC]  fill: 44160/sec   reads: 172/sec  avg bytes/read: 512  ring: ~2000
```

---

## Next Steps

1. **Flash and reconnect** — check if calls: 335/sec appears in log
2. **If 335/sec restored:** Fix the 7-8s periodic gaps
   - Increase RING_SIZE from 4096 to 8192 (more buffer against brief BT stutters)
   - Investigate if gap correlates with WiFi stop/start or BT heartbeat
3. **If still 167/sec after packet type fix:**
   - Try completely different approach: reduce I2S fill rate to match 167/sec drain rate
   - `kI2SConfigRate` = ~26000 Hz (→ actual 21,320 Hz fill), `kResampleRatio` = 1.003
   - Declare 44100 Hz to headset — audio tempo will be wrong; need to also change declared sample rate
   - OR: accept lower quality by declaring 16000 Hz SBC (within DH5 capacity)

---

## Key Files
| File | Purpose |
|------|---------|
| `src/main.cpp` | All application logic |
| `.pio/libdeps/lolin32/ESP32-A2DP/src/BluetoothA2DPSource.cpp` | BT library (modified) |
| `tools/monitor.py` | Serial monitor → `docs/logs/live.log` |
| `docs/logs/live.log` | Live serial output log |
| `PROGRESS.md` | This file |

## How to Start Monitor
```
python tools/monitor.py COM7 115200
```
Check `docs/logs/live.log` for output.
