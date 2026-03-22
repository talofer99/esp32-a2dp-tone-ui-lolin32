#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <BluetoothA2DPSource.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include "soc/syscon_struct.h"
#include <esp_wifi.h>
#include <esp_gap_bt_api.h>
#include <nvs_flash.h>
#include "test_audio.h"  // PCM test samples — regenerate with: python tools/make_test_audio.py

// ---- Firmware version ----
static const char *kFwVersion = "1.0.10";

// ---- Constants ----
static const char *kApSsid     = "ESP32-Audio-Setup";
static const char *kApPassword = "esp32audio";
static const uint32_t kSampleRate = 44100;
// I2S ADC built-in mode runs at ~82% of the configured rate (36,096 Hz at 44,100 config).
// Configure at 44,100 / 0.8185 ≈ 53,878 Hz so the hardware delivers ~44,100 Hz actual.
// This corrects the 1.22x pitch shift and keeps fill > drain (no ring underruns).
static const uint32_t kI2SConfigRate = 53878;
static const adc1_channel_t kAdcChannelL = ADC1_CHANNEL_6;  // GPIO34 — left
static const adc1_channel_t kAdcChannelR = ADC1_CHANNEL_7;  // GPIO35 — right
static const i2s_port_t kI2SPort        = I2S_NUM_0;
static const int kAdcMidpoint           = 2048;

// Reconnect backoff (ms)
static const unsigned long kReconnectSteps[] = {2000, 4000, 8000, 20000, 60000, 120000};
static const int kReconnectStepCount = 6;

// ---- Ring buffers — left (CH6/GPIO34) and right (CH7/GPIO35) ----
// Core 1 writes, Core 0 (BT callback) reads.
#define RING_SIZE 4096
static int16_t  ringBuf[RING_SIZE];    // left channel
static volatile int ringWrite = 0;
static volatile int ringRead  = 0;

static int16_t  ringBufR[RING_SIZE];   // right channel
static volatile int ringWriteR = 0;
static volatile int ringReadR  = 0;

// ---- State ----
BluetoothA2DPSource a2dp_source;
WebServer  server(80);
DNSServer  dnsServer;
Preferences prefs;

String targetDeviceName = "";
bool   sourceStarted    = false;
bool   toneEnabled      = false;
volatile bool btConnected = false;
String lastBtState      = "idle";
volatile int audioVolume = 80;
bool   adcEnabled       = false;  // tracks whether i2s_adc_enable() has been called
static esp_adc_cal_characteristics_t adcChars;  // calibration data loaded in setupADC()
bool   testToneMode     = false;  // false = ADC input (default); true = embedded test audio

int  reconnectStepIdx = 0;
bool reconnectPending = false;
unsigned long reconnectAt = 0;

// Stereo mode: true once right channel (CH7) is confirmed receiving samples from SYSCON pattern.
// getDataFrames uses this to select stereo ratio (~0.5) vs mono ratio (~1.0).
static volatile bool stereoActive = false;

// DH5 auto-reconnect: if BT callbacks stuck at ~167/sec (DH5) for 10s, disconnect and
// let auto_reconnect renegotiate — sometimes the new connection uses DH3 (335+/sec).
static volatile bool triggerDH5Reconnect = false;
static volatile int  dh5ReconnectCount   = 0;
static const int     kMaxDH5Reconnects   = 5;  // give up after 5 attempts per session


// ---- Ring buffer helpers — left ----
static inline int ringAvail() {
  int w = ringWrite, r = ringRead;
  return (w >= r) ? (w - r) : (RING_SIZE - r + w);
}

static inline void ringPush(int16_t v) {
  int next = (ringWrite + 1) % RING_SIZE;
  if (next != ringRead) {
    ringBuf[ringWrite] = v;
    ringWrite = next;
  }
}

static inline int16_t ringPop() {
  int16_t v = ringBuf[ringRead];
  ringRead = (ringRead + 1) % RING_SIZE;
  return v;
}

// ---- Ring buffer helpers — right ----
static inline int ringRAvail() {
  int w = ringWriteR, r = ringReadR;
  return (w >= r) ? (w - r) : (RING_SIZE - r + w);
}

static inline void ringRPush(int16_t v) {
  int next = (ringWriteR + 1) % RING_SIZE;
  if (next != ringReadR) {
    ringBufR[ringWriteR] = v;
    ringWriteR = next;
  }
}

static inline int16_t ringRPop() {
  int16_t v = ringBufR[ringReadR];
  ringReadR = (ringReadR + 1) % RING_SIZE;
  return v;
}

// ---- ADC sampling task — Core 1 ----
// kMidpointMv: expected DC bias at ADC pin with 2×100kΩ divider on 3.3V = 1650 mV
static const int32_t kMidpointMv = 1650;

void adcTask(void *) {
  // I2S ADC built-in: each 16-bit word = [15:12] channel ID, [11:0] 12-bit value.
  // SYSCON pattern table: written here (Core 1) once ADC is enabled, safely after BT init.
  // A11: writing SYSCON before BT init (in setupADC) blocks the data callback — do it here.
  static uint16_t tmp[256];
  bool sysconApplied = false;
  while (true) {
    // Apply SYSCON stereo pattern once, after i2s_adc_enable has been called
    if (!sysconApplied && adcEnabled) {
      SYSCON.saradc_ctrl.sar1_patt_len = 1;
      SYSCON.saradc_sar1_patt_tab[0] = (0x6F << 24) | (0x7F << 16) | (0x6F << 8) | 0x7F;
      sysconApplied = true;
      // Step 22: flush ring so getDataFrames starts popping from clean L,R,L,R alignment.
      // Without this, pre-SYSCON CH6-only samples cause misaligned (L,L) pairs.
      ringWrite = 0; ringRead = 0;
      Serial.println("[ADC] SYSCON stereo pattern applied (CH6=L, CH7=R), ring flushed");
    }
    size_t bytes_read = 0;
    i2s_read(kI2SPort, tmp, sizeof(tmp), &bytes_read, pdMS_TO_TICKS(10));
    int n = bytes_read / sizeof(uint16_t);
    static uint32_t fillLCount = 0;
    static uint32_t fillRCount = 0;
    static uint32_t readCount  = 0;
    static uint32_t fillTick   = 0;
    // Step 22: hold CH6 until CH7 arrives, then push the complete L+R pair together.
    // Guarantees ring is always L,R,L,R aligned — no stray CH6-only samples between pairs.
    static int16_t pendingL    = 0;
    static bool    hasPendingL = false;
    readCount++;
    for (int i = 0; i < n; i++) {
      uint8_t  chId = (tmp[i] >> 12) & 0xF;
      // Step 26: use eFuse calibration instead of linear conversion (fixes ADC nonlinearity)
      uint32_t mv   = esp_adc_cal_raw_to_voltage(tmp[i] & 0x0FFF, &adcChars);
      int32_t  adc  = (int32_t)mv - kMidpointMv;
      // Step 29 REVERTED — soft gate degraded music quality; ADC noise is hardware floor
      if (chId == 6) {
        pendingL    = (int16_t)adc;
        hasPendingL = true;
        fillLCount++;
      } else if (chId == 7 && hasPendingL) {
        ringPush(pendingL);          // L first
        ringPush((int16_t)adc);      // R second
        hasPendingL = false;
        fillRCount++;
      }
    }
    uint32_t now = millis();
    if (now - fillTick >= 2000) {
      uint32_t lRate = fillLCount / 2, rRate = fillRCount / 2;
      // Confirm stereo once R channel reaches ≥10% of L rate (SYSCON pattern working)
      if (!stereoActive && rRate > lRate / 10 && lRate > 0) {
        stereoActive = true;
        Serial.println("[ADC] Stereo confirmed — CH7 (GPIO35) active");
      }
      Serial.printf("[v%s][ADC] L: %u/sec  R: %u/sec  reads: %u/sec  ringL: %d  ringR: %d  stereo: %s\n",
                    kFwVersion, lRate, rRate, readCount / 2, ringAvail(), ringRAvail(), stereoActive ? "yes" : "no");
      fillLCount = 0; fillRCount = 0; readCount = 0; fillTick = now;
    }
  }
}

// ---- A2DP audio callback — called from Core 0 BT task ----
// Step 21: single interleaved ring. adcTask pushes CH6(L) and CH7(R) both to ringBuf.
// Ring contains: L, R, L, R, ... (SYSCON alternates CH6/CH7)
// getDataFrames pops 2 per output frame: first = L (channel1), second = R (channel2).
// No ringBufR access in BT task — avoids the A13 mystery crash.
int32_t getDataFrames(Frame *frame, int32_t frame_count) {
  int vol = audioVolume;
  bool active = toneEnabled && btConnected;

  // ---- Test mode: play embedded PCM samples (looped) ----
  if (active && testToneMode) {
    static int32_t audioPos = 0;
    for (int i = 0; i < frame_count; i++) {
      int16_t s = (int16_t)((int32_t)kTestAudio[audioPos % kTestAudioLen] * vol / 100);
      frame[i].channel1 = s;
      frame[i].channel2 = s;
      audioPos++;
    }
    return frame_count;
  }

  // ---- ADC mode: stereo, single interleaved ring ----
  // Ring fills at ~22080 pairs/sec (L,R,L,R interleaved from adcTask pairing).
  // BT drain rate: ~335 calls/sec × 108 frames = ~36,180 output pairs/sec.
  // Without resampler drain >> fill → ring empties → silence gaps → buzzing.
  // Resampler: phase accumulator advances by kResampleRatio per output frame.
  // When phase ≥ 1.0 we pop a new L,R pair from ring (rsCurr advances).
  // Linear interpolation between rsPrev and rsCurr for both channels.
  // Adaptive ratio: adjust every 2s to keep ring near RING_SIZE/4 pairs.
  static int16_t rsPrevL = 0, rsCurrL = 0;
  static int16_t rsPrevR = 0, rsCurrR = 0;
  static float   rsPhase = 0.0f;
  static float   kRatio  = 0.515f;  // fill_pairs/drain_pairs ≈ 22080/42880; adaptive

  static uint32_t drainCount = 0;  // output frames produced
  static uint32_t callCount  = 0;
  static uint32_t drainTick  = 0;

  callCount++;
  for (int i = 0; i < frame_count; i++) {
    if (active) {
      // Advance phase; pop ring pair when crossing integer boundary
      while (rsPhase >= 1.0f) {
        if (ringAvail() >= 2) {
          rsPrevL = rsCurrL;  rsPrevR = rsCurrR;
          rsCurrL = ringPop();  // L (CH6)
          rsCurrR = ringPop();  // R (CH7)
        }
        rsPhase -= 1.0f;
      }
      // Linear interpolation
      int32_t sL = (int32_t)rsPrevL + (int32_t)(((int32_t)rsCurrL - rsPrevL) * rsPhase);
      int32_t sR = (int32_t)rsPrevR + (int32_t)(((int32_t)rsCurrR - rsPrevR) * rsPhase);
      frame[i].channel1 = (int16_t)(sL * 12 * vol / 100);  // step 28: 19→12, more headroom
      frame[i].channel2 = (int16_t)(sR * 12 * vol / 100);
      rsPhase += kRatio;
      drainCount++;
    } else {
      frame[i].channel1 = 0;
      frame[i].channel2 = 0;
    }
  }

  uint32_t now = millis();
  if (now - drainTick >= 2000) {
    // v1.0.7: compute ratio directly from measured drain rate
    // Fill is ~22080 pairs/sec (known from ADC). ratio = fill / drain.
    uint32_t drainPerSec = drainCount / 2;
    if (drainPerSec > 1000) {
      float measured = 22080.0f / (float)drainPerSec;
      // Blend: 80% measured, 20% previous (smooth transitions)
      kRatio = 0.8f * measured + 0.2f * kRatio;
    }
    // Fine-tune: nudge based on ring level
    int avail = ringAvail();
    int target = RING_SIZE / 4;
    if      (avail > target + 512) kRatio *= 1.001f;
    else if (avail < target - 512) kRatio *= 0.999f;
    if (kRatio > 1.20f) kRatio = 1.20f;
    if (kRatio < 0.40f) kRatio = 0.40f;

    Serial.printf("[A2DP] drain: %u/sec  calls: %u/sec  avg: %u  ring: %d  ratio: %.4f  act=%d\n",
                  drainCount / 2, callCount / 2,
                  callCount ? drainCount / callCount : 0, ringAvail(),
                  kRatio, (int)active);
    if (callCount > 0) {
      static int dh5Streak = 0;
      uint32_t callsPerSec = callCount / 2;
      // DH5 reconnect disabled in v1.0.5 — some headsets only support DH5 (167/sec).
      // Adaptive resampler handles the lower drain rate; reconnect loop was preventing playback.
      if (callsPerSec >= 250) {
        dh5ReconnectCount = 0;
      }
    }
    drainCount = 0; callCount = 0; drainTick = now;
  }
  return frame_count;
}

// ---- BT state callback ----
void scheduleReconnect() {
  unsigned long ms = kReconnectSteps[reconnectStepIdx];
  if (reconnectStepIdx < kReconnectStepCount - 1) reconnectStepIdx++;
  reconnectAt     = millis() + ms;
  reconnectPending = true;
  Serial.printf("[BT] next attempt in %lus\n", ms / 1000);
}

void enableADC() {
  if (!adcEnabled) {
    i2s_adc_enable(kI2SPort);
    adcEnabled = true;
    Serial.println("[ADC] I2S enabled");
  }
}

// Called when A2DP audio stream actually starts — fallback if connectionStateChanged
// doesn't fire (library sometimes transitions via audio event instead of conn event)
void audioStateChanged(esp_a2d_audio_state_t state, void *ptr) {
  (void)ptr;
  if (state == ESP_A2D_AUDIO_STATE_STARTED) {
    if (!btConnected) {
      btConnected = true;
      lastBtState = "connected";
    }
    toneEnabled = true;
    Serial.println("[Audio] streaming started");
  }
}

void connectionStateChanged(esp_a2d_connection_state_t state, void *ptr) {
  (void)ptr;
  lastBtState = a2dp_source.to_str(state);
  btConnected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  Serial.printf("[A2DP] state: %s\n", lastBtState.c_str());

  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    // Always stop WiFi on connect — WiFi + BT together exhaust heap, SBC can't init
    esp_wifi_stop();

    // v1.0.10: BT bandwidth tricks
    // 1) Disable BT controller sleep — keep radio active for max throughput
    esp_err_t sleep_err = esp_bt_sleep_disable();
    Serial.printf("[BT] sleep_disable → %s\n", esp_err_to_name(sleep_err));

    // 2) Stop being discoverable/connectable — free up scan slots for data
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    Serial.println("[BT] scan mode → non-connectable/non-discoverable");

    // 3) Request DH3 packet type + enable EDR
    esp_bd_addr_t peer;
    memcpy(peer, a2dp_source.get_last_peer_address(), sizeof(esp_bd_addr_t));
    esp_err_t pkt_err = esp_bt_gap_set_acl_pkt_types(peer,
        ESP_BT_ACL_PKT_TYPES_MASK_DH3 |
        ESP_BT_ACL_PKT_TYPES_MASK_DM3 |
        ESP_BT_ACL_PKT_TYPES_MASK_DM1 |
        ESP_BT_ACL_PKT_TYPES_MASK_DH1);
    Serial.printf("[BT] set_acl_pkt_types → %s\n", esp_err_to_name(pkt_err));

    toneEnabled = true;
    if (!testToneMode) enableADC();
    Serial.println("[Audio] streaming enabled, WiFi stopped");
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    toneEnabled = false;
    btConnected = false;
    if (adcEnabled) {
      i2s_adc_disable(kI2SPort);
      adcEnabled = false;
    }
    esp_wifi_start();
    Serial.println("[Audio] stopped, WiFi restarted");
  }
}

void startA2DP() {
  if (sourceStarted || targetDeviceName.isEmpty()) return;
  // v1.0.10: disable BT sleep before starting — maximize radio availability
  esp_bt_sleep_disable();
  a2dp_source.set_auto_reconnect(true);  // library handles reconnects — no re-init
  a2dp_source.set_volume(30);
  a2dp_source.set_on_connection_state_changed(connectionStateChanged);
  a2dp_source.set_on_audio_state_changed(audioStateChanged);  // fallback if conn event missed
  a2dp_source.set_data_callback_in_frames(getDataFrames);
  Serial.printf("[A2DP] starting -> %s\n", targetDeviceName.c_str());
  a2dp_source.start(targetDeviceName.c_str());
  sourceStarted = true;
  lastBtState   = "starting";
}

// ---- HTML helpers ----
String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

String renderPage() {
  String page;
  page.reserve(4096);
  page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>ESP32 A2DP Audio</title><style>");
  page += F("body{font-family:Arial,sans-serif;max-width:760px;margin:24px auto;padding:0 16px;background:#f7f7f7;color:#222}");
  page += F(".card{background:#fff;border-radius:12px;padding:18px;box-shadow:0 2px 10px rgba(0,0,0,.08);margin-bottom:16px}");
  page += F("input,button{font-size:16px;padding:10px 12px;border-radius:10px;border:1px solid #ccc}");
  page += F("button{cursor:pointer;background:#111;color:#fff;border:none;margin-right:8px;margin-top:8px}");
  page += F("button.danger{background:#c0392b}");
  page += F("input[type=range]{width:100%;padding:4px 0;border:none}");
  page += F(".muted{color:#666}.ok{color:#0a7d32}.warn{color:#9a6700}.mono{font-family:monospace}</style></head><body>");

  page += F("<div class='card'><h2>ESP32 A2DP Audio</h2>");
  page += F("<p class='muted'>Stream analog input (GPIO34) to a Bluetooth headset.</p></div>");

  page += F("<div class='card'><h3>Status</h3>");
  page += F("<p><strong>Target:</strong> ");
  page += htmlEscape(targetDeviceName.length() ? targetDeviceName : String("(not set)"));
  page += F("</p><p><strong>Bluetooth:</strong> <span id='bt'>");
  page += htmlEscape(lastBtState);
  page += F("</span></p><p><strong>Connected:</strong> <span id='conn' class='");
  page += btConnected ? F("ok'>yes") : F("warn'>no");
  page += F("</span></p><p><strong>Streaming:</strong> <span id='streaming'>");
  page += toneEnabled ? F("on") : F("off");
  page += F("</span></p></div>");

  page += F("<div class='card'><h3>1) Connect headset</h3>");
  page += F("<input type='text' id='devname' placeholder='Exact Bluetooth name' style='width:100%;box-sizing:border-box;margin-bottom:8px' value='");
  page += htmlEscape(targetDeviceName);
  page += F("'>");
  page += F("<div><button id='connbtn' onclick=\"");
  page += F("var n=document.getElementById('devname').value.trim();");
  page += F("if(!n)return;");
  page += F("document.getElementById('connbtn').textContent='Connecting...';");
  page += F("document.getElementById('connbtn').disabled=true;");
  page += F("fetch('/connect?name='+encodeURIComponent(n)).then(r=>r.json()).then(function(d){");
  page += F("document.getElementById('connbtn').textContent='Connect';");
  page += F("document.getElementById('connbtn').disabled=false;");
  page += F("document.getElementById('connmsg').textContent=d.msg;");
  page += F("}).catch(function(){");
  page += F("document.getElementById('connbtn').textContent='Connect';");
  page += F("document.getElementById('connbtn').disabled=false;");
  page += F("});\">Connect</button>");
  if (targetDeviceName.length()) {
    page += F("<a href='/forget'><button type='button' class='danger'>Forget device</button></a>");
  }
  page += F("<a href='/fullreset'><button type='button' class='danger'>Full BT reset</button></a>");
  page += F("</div>");
  page += F("<p id='connmsg' class='muted'></p>");
  page += F("<p class='muted'>Put headset in pairing mode. Audio starts automatically when connected.</p></div>");

  page += F("<div class='card'><h3>2) Manual audio control</h3>");
  page += F("<button onclick=\"fetch('/tone/on')\">Start audio</button>");
  page += F("<button onclick=\"fetch('/tone/off')\">Stop audio</button>");
  page += F("<hr style='margin:12px 0'>");
  page += F("<p class='muted' style='margin:0 0 8px'><b>Test audio</b> (embedded PCM chord, no ADC) — use to verify BT quality. WiFi stays up.<br><b>ADC input</b> — switches to live analog input. WiFi stops to reduce interference.</p>");
  page += F("<button onclick=\"fetch('/testtone/on')\">&#9654; Switch to Test audio</button>");
  page += F("<button onclick=\"fetch('/testtone/off')\">&#127908; Switch to ADC input</button>");
  page += F("</div>");

  page += F("<div class='card'><h3>3) Volume</h3>");
  page += F("<input type='range' id='vol' min='0' max='100' value='");
  page += String(audioVolume);
  page += F("'> <span id='volval'>");
  page += String(audioVolume);
  page += F("%</span>");
  page += F("<script>");
  page += F("var s=document.getElementById('vol');");
  page += F("s.oninput=function(){document.getElementById('volval').textContent=this.value+'%';};");
  page += F("s.onchange=function(){fetch('/volume?level='+this.value);};");
  page += F("</script></div>");

  // Status polling
  page += F("<script>");
  page += F("function refresh(){fetch('/status').then(r=>r.json()).then(function(d){");
  page += F("document.getElementById('bt').textContent=d.bt;");
  page += F("document.getElementById('conn').textContent=d.connected?'yes':'no';");
  page += F("document.getElementById('conn').className=d.connected?'ok':'warn';");
  page += F("document.getElementById('streaming').textContent=d.streaming?'on':'off';");
  page += F("}).catch(()=>{});}");
  page += F("setInterval(refresh,3000);");
  page += F("</script>");
  page += F("</body></html>");
  return page;
}

// ---- HTTP handlers ----
void handleRoot()     { server.send(200, "text/html", renderPage()); }

void handleRedirect() {
  server.sendHeader("Location", "http://192.168.4.1/");
  server.sendHeader("Connection", "close");
  server.send(302, "text/plain", "");
}

void handleStatus() {
  String json = "{\"bt\":\"";
  json += lastBtState;
  json += "\",\"connected\":";
  json += btConnected ? "true" : "false";
  json += ",\"streaming\":";
  json += toneEnabled ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void handleConnect() {
  if (server.hasArg("name")) {
    targetDeviceName = server.arg("name");
    targetDeviceName.trim();
  }
  if (targetDeviceName.isEmpty()) {
    server.send(400, "application/json", "{\"msg\":\"Missing device name\"}");
    return;
  }
  prefs.begin("audio", false);
  prefs.putString("device", targetDeviceName);
  prefs.end();

  String msg;
  if (!sourceStarted) {
    startA2DP();
    msg = "Scanning for " + targetDeviceName + "...";
  } else {
    msg = "Already connecting — check status above.";
  }
  server.send(200, "application/json", "{\"msg\":\"" + msg + "\"}");
}

void handleForget() {
  prefs.begin("audio", false);
  prefs.remove("device");
  prefs.end();
  server.send(200, "text/html",
    "<html><body><p>Device forgotten. Rebooting...</p>"
    "<script>setTimeout(function(){location='/'}, 4000);</script></body></html>");
  delay(500);
  ESP.restart();
}

void handleFullReset() {
  server.send(200, "text/html",
    "<html><body><p>Full NVS reset (clears BT bonds + all settings). Rebooting...</p>"
    "<script>setTimeout(function(){location='/'}, 6000);</script></body></html>");
  delay(500);
  nvs_flash_erase();   // wipes Bluedroid BT state + our prefs — forces fresh BT negotiation
  ESP.restart();
}

void handleToneOn()  { toneEnabled = true;  server.send(200, "text/plain", "ok"); }
void handleToneOff() { toneEnabled = false; server.send(200, "text/plain", "ok"); }
void handleTestToneOn()  { testToneMode = true;  server.send(200, "text/plain", "ok"); }
void handleTestToneOff() {
  testToneMode = false;
  if (btConnected) enableADC();  // switching to ADC mode — enable I2S ADC
  server.send(200, "text/plain", "ok");
}

void handleVolume() {
  if (server.hasArg("level")) {
    int v = server.arg("level").toInt();
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    audioVolume = v;
    prefs.begin("audio", false);
    prefs.putInt("volume", v);
    prefs.end();
  }
  server.send(200, "text/plain", "ok");
}

// ---- Setup functions ----
void setupADC() {
  // Install I2S driver in setup() — safe with ONLY_LEFT format before BT init.
  // (RIGHT_LEFT format caused BT crash loop — A8/A10. ONLY_LEFT is fine here.)
  i2s_config_t cfg      = {};
  cfg.mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN);
  cfg.sample_rate       = kI2SConfigRate;
  cfg.bits_per_sample   = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format    = I2S_CHANNEL_FMT_ONLY_LEFT;  // mono-width; demux CH6/CH7 by channel ID
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_desc_num      = 4;
  cfg.dma_frame_num     = 256;
  cfg.use_apll          = false;

  // Set input range to 0–3.3V so 1.65V bias is centred (default 0dB = 0–1.1V clips!)
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(kAdcChannelL, ADC_ATTEN_DB_11);  // GPIO34 — left
  adc1_config_channel_atten(kAdcChannelR, ADC_ATTEN_DB_11);  // GPIO35 — right
  // Load per-chip eFuse calibration — corrects ADC nonlinearity
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 0, &adcChars);

  i2s_driver_install(kI2SPort, &cfg, 0, NULL);
  i2s_set_adc_mode(ADC_UNIT_1, kAdcChannelL);
  // SYSCON pattern table is written from adcTask (Core 1) once i2s_adc_enable fires,
  // NOT here — writing SYSCON before BT init appears to block the data callback.
  // i2s_adc_enable() is called later in connectionStateChanged, AFTER BT connects.
  Serial.println("[ADC] I2S configured (SYSCON stereo pattern applied from adcTask after enable)");

  xTaskCreatePinnedToCore(adcTask, "adc_task", 4096, NULL, 5, NULL, 1);
  Serial.println("[ADC] sampling task on Core 1");
}

void setupWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid, kApPassword);
  esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("[WiFi] AP up: %s\n", WiFi.softAPIP().toString().c_str());

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/",             HTTP_GET, handleRoot);
  server.on("/connect",      HTTP_GET, handleConnect);
  server.on("/forget",       HTTP_GET, handleForget);
  server.on("/fullreset",    HTTP_GET, handleFullReset);
  server.on("/tone/on",      HTTP_GET, handleToneOn);
  server.on("/tone/off",     HTTP_GET, handleToneOff);
  server.on("/testtone/on",  HTTP_GET, handleTestToneOn);
  server.on("/testtone/off", HTTP_GET, handleTestToneOff);
  server.on("/volume",       HTTP_GET, handleVolume);
  server.on("/status",       HTTP_GET, handleStatus);

  server.on("/hotspot-detect.html",       HTTP_GET, handleRedirect);
  server.on("/library/test/success.html", HTTP_GET, handleRedirect);
  server.on("/generate_204",              HTTP_GET, handleRedirect);
  server.on("/gen_204",                   HTTP_GET, handleRedirect);
  server.on("/ncsi.txt",                  HTTP_GET, handleRedirect);
  server.on("/connecttest.txt",           HTTP_GET, handleRedirect);
  server.onNotFound(handleRedirect);

  server.begin();
}

// ---- Arduino entry points ----
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== ESP32 A2DP Audio v%s ===\n", kFwVersion);
  Serial.printf("Running setup on Core %d\n", xPortGetCoreID());

  prefs.begin("audio", false);
  targetDeviceName = prefs.getString("device", "");
  audioVolume      = prefs.getInt("volume", 80);
  prefs.end();

  if (targetDeviceName.length()) {
    Serial.printf("[Prefs] last device: %s  volume: %d\n",
                  targetDeviceName.c_str(), (int)audioVolume);
  }

  setupADC();   // I2S driver + SYSCON stereo pattern + ADC task on Core 1
  setupWeb();   // WiFi AP + HTTP server

  if (!targetDeviceName.isEmpty()) {
    startA2DP();  // auto-connect to last known device on boot
  }

  Serial.println("http://192.168.4.1  (ESP32-Audio-Setup / esp32audio)");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // DH5 auto-reconnect: triggered from getDataFrames after 10s of choppy BT rate.
  // Two-phase: disconnect() to close DH5 ACL, then reconnect() 3s later for fresh negotiation.
  // NOTE: disconnect() sets is_autoreconnect_allowed=false — must call reconnect() manually.
  static unsigned long dh5ReconnectPhase2At = 0;

  if (triggerDH5Reconnect) {
    triggerDH5Reconnect = false;
    dh5ReconnectCount++;
    Serial.printf("[BT] DH5 reconnect attempt %d/%d — disconnecting\n",
                  (int)dh5ReconnectCount, kMaxDH5Reconnects);
    a2dp_source.disconnect();
    dh5ReconnectPhase2At = millis() + 3000;  // wait 3s for ACL teardown before reconnecting
  }

  if (dh5ReconnectPhase2At > 0 && millis() >= dh5ReconnectPhase2At) {
    dh5ReconnectPhase2At = 0;
    Serial.println("[BT] reconnecting after DH5 disconnect");
    a2dp_source.reconnect();  // resets is_autoreconnect_allowed=true, tries fresh ACL
  }

  delay(10);
}
