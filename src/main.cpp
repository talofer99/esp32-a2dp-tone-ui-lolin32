#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <BluetoothA2DPSource.h>
#include <esp_adc/adc_continuous.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_wifi.h>
#include <esp_gap_bt_api.h>
#include <nvs_flash.h>

// ---- Firmware version ----
static const char *kFwVersion = "1.0.14";

// Uncomment to enable periodic ADC/A2DP stats on serial (every 2s)
// #define DEBUG_STATS

// ---- Constants ----
static const char *kApSsid     = "ESP32-Audio-Setup";
static const char *kApPassword = "esp32audio";
static const uint32_t kSampleRate = 44100;
// adc_continuous sample rate: total conversions/sec across both channels
// ESP32 ADC has ~82% efficiency (same as legacy I2S ADC mode).
// 53878 configured → ~44,160 actual → ~22,080/sec per channel
static const uint32_t kAdcSampleRate = 53878;
static const int kAdcChannelL = ADC_CHANNEL_6;  // GPIO34 — left
static const int kAdcChannelR = ADC_CHANNEL_7;  // GPIO35 — right

// ---- LED state indication (v1.0.13) ----
static const int kLedPin     = 5;       // GPIO5 = LED_BUILTIN on LOLIN32
static const int kLedFreq    = 5000;    // PWM frequency
static const int kLedRes     = 8;       // 8-bit resolution (0-255)

enum LedState { LED_OFF, LED_BREATHING, LED_SLOW_BLINK, LED_SOLID, LED_RAPID_BLINK };
static LedState ledState = LED_OFF;
static unsigned long ledRapidStart = 0;  // millis() when rapid blink started

// v1.0.12: Fast reconnect — MAC first, then name scan, then give up
static const int kMaxMacAttempts  = 3;   // ~7s each = ~21s max
static const int kMaxNameAttempts = 2;   // ~18s each = ~36s max
static const unsigned long kConnectTimeout = 15000;  // ms per attempt before giving up

// ---- Ring buffers — left (CH6/GPIO34) and right (CH7/GPIO35) ----
// Core 1 writes, Core 0 (BT callback) reads.
#define RING_SIZE 4096
static int16_t  ringBuf[RING_SIZE];    // left channel
static volatile int ringWrite = 0;
static volatile int ringRead  = 0;


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
bool   adcEnabled       = false;  // tracks whether adc_continuous_start() has been called
static adc_continuous_handle_t adcHandle = NULL;
static adc_cali_handle_t adcCaliHandle = NULL;

// v1.0.12: connection retry state
int  macAttempts     = 0;
int  nameAttempts    = 0;
bool connectingPhase = false;   // true while attempting to connect
unsigned long connectStartedAt = 0;  // millis() when current attempt began
bool wifiFallbackDone = false;  // true once we gave up and started WiFi
bool bootHasSavedDevice = false; // true if we booted with a saved device (skip WiFi initially)

// Stereo mode: true once right channel (CH7) is confirmed receiving samples from SYSCON pattern.
// getDataFrames uses this to select stereo ratio (~0.5) vs mono ratio (~1.0).
static volatile bool stereoActive = false;

// DH5 reconnect disabled in v1.0.5 — esp_bt_sleep_disable() fixed the issue in v1.0.10
static volatile bool resetResampler = false;  // v1.0.13: signal getDataFrames to reset state


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


// ---- ADC sampling task — Core 1 ----
// kMidpointMv: expected DC bias at ADC pin with 2×100kΩ divider on 3.3V = 1650 mV
static const int32_t kMidpointMv = 1650;

void adcTask(void *) {
  // adc_continuous output: each result is adc_digi_output_data_t (2 bytes on ESP32)
  // type1 format: [15:12] channel, [11:0] 12-bit raw value
  // Channel scan pattern alternates CH6(L) → CH7(R) → CH6(L) → CH7(R)...
  static uint8_t buf[512];  // read buffer (256 results × 2 bytes each)
  while (true) {
    if (!adcEnabled || adcHandle == NULL) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    uint32_t bytes_read = 0;
    esp_err_t err = adc_continuous_read(adcHandle, buf, sizeof(buf), &bytes_read, 10);
    if (err != ESP_OK) {
      continue;
    }
    int n = bytes_read / sizeof(adc_digi_output_data_t);
    adc_digi_output_data_t *results = (adc_digi_output_data_t *)buf;

    static uint32_t fillLCount = 0;
    static uint32_t fillRCount = 0;
    static uint32_t readCount  = 0;
    static uint32_t fillTick   = 0;
    // Hold CH6 until CH7 arrives, then push the complete L+R pair together.
    static int16_t pendingL    = 0;
    static bool    hasPendingL = false;
    readCount++;
    for (int i = 0; i < n; i++) {
      uint8_t  chId = results[i].type1.channel;
      uint16_t raw  = results[i].type1.data;
      // eFuse calibration: convert raw → millivolts (corrects ADC nonlinearity)
      int mv = 0;
      if (adcCaliHandle) {
        adc_cali_raw_to_voltage(adcCaliHandle, raw, &mv);
      }
      int32_t adc = (int32_t)mv - kMidpointMv;
      if (chId == kAdcChannelL) {
        pendingL    = (int16_t)adc;
        hasPendingL = true;
        fillLCount++;
      } else if (chId == kAdcChannelR && hasPendingL) {
        ringPush(pendingL);          // L first
        ringPush((int16_t)adc);      // R second
        hasPendingL = false;
        fillRCount++;
      }
    }
    uint32_t now = millis();
    if (now - fillTick >= 2000) {
      uint32_t lRate = fillLCount / 2, rRate = fillRCount / 2;
      if (!stereoActive && rRate > lRate / 10 && lRate > 0) {
        stereoActive = true;
        Serial.println("[ADC] Stereo confirmed — CH7 (GPIO35) active");
      }
#ifdef DEBUG_STATS
      Serial.printf("[v%s][ADC] L: %u/sec  R: %u/sec  reads: %u/sec  ring: %d  stereo: %s\n",
                    kFwVersion, lRate, rRate, readCount / 2, ringAvail(), stereoActive ? "yes" : "no");
#endif
      fillLCount = 0; fillRCount = 0; readCount = 0; fillTick = now;
    }
  }
}

// ---- A2DP audio callback — called from Core 0 BT task ----
// Step 21: single interleaved ring. adcTask pushes CH6(L) and CH7(R) both to ringBuf.
// Ring contains: L, R, L, R, ... (SYSCON alternates CH6/CH7)
// getDataFrames pops 2 per output frame: first = L (channel1), second = R (channel2).
int32_t getDataFrames(Frame *frame, int32_t frame_count) {
  int vol = audioVolume;
  bool active = toneEnabled && btConnected;

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

  // v1.0.13: reset resampler on reconnect — stale ratio causes unstable audio
  if (resetResampler) {
    rsPrevL = rsCurrL = rsPrevR = rsCurrR = 0;
    rsPhase = 0.0f;
    kRatio = 0.515f;
    drainCount = 0; callCount = 0; drainTick = millis();
    resetResampler = false;
  }

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

#ifdef DEBUG_STATS
    Serial.printf("[A2DP] drain: %u/sec  calls: %u/sec  avg: %u  ring: %d  ratio: %.4f  act=%d\n",
                  drainCount / 2, callCount / 2,
                  callCount ? drainCount / callCount : 0, ringAvail(),
                  kRatio, (int)active);
#endif
    drainCount = 0; callCount = 0; drainTick = now;
  }
  return frame_count;
}

// ---- BT state callback ----

void enableADC() {
  if (!adcEnabled && adcHandle != NULL) {
    esp_err_t err = adc_continuous_start(adcHandle);
    if (err == ESP_OK) {
      adcEnabled = true;
      // Flush ring for clean L,R alignment on start
      ringWrite = 0; ringRead = 0;
      Serial.println("[ADC] continuous mode started");
    } else {
      Serial.printf("[ADC] start failed: %s\n", esp_err_to_name(err));
    }
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
    // v1.0.12: connection succeeded — stop retry tracking
    connectingPhase = false;
    macAttempts = 0;
    nameAttempts = 0;

    // Stop WiFi — WiFi + BT together exhaust heap, SBC can't init
    esp_wifi_stop();

    // v1.0.10: BT bandwidth tricks
    esp_err_t sleep_err = esp_bt_sleep_disable();
    Serial.printf("[BT] sleep_disable → %s\n", esp_err_to_name(sleep_err));
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    esp_bd_addr_t peer;
    memcpy(peer, a2dp_source.get_last_peer_address(), sizeof(esp_bd_addr_t));
    esp_bt_gap_set_acl_pkt_types(peer,
        ESP_BT_ACL_PKT_TYPES_MASK_DH3 |
        ESP_BT_ACL_PKT_TYPES_MASK_DM3 |
        ESP_BT_ACL_PKT_TYPES_MASK_DM1 |
        ESP_BT_ACL_PKT_TYPES_MASK_DH1);

    // v1.0.12: save MAC for fast reconnect on next boot
    // (library saves it too, but we log it for visibility)
    Serial.printf("[BT] connected to MAC: %s\n", a2dp_source.to_str(peer));

    // v1.0.13: flush ring + reset resampler on (re)connect — discard stale audio & ratio
    ringWrite = 0; ringRead = 0;
    resetResampler = true;

    toneEnabled = true;
    enableADC();
    Serial.println("[Audio] streaming enabled, WiFi stopped");
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    toneEnabled = false;
    btConnected = false;
    // Don't disable ADC on disconnect — keep ring pre-filled for fast reconnect
    // v1.0.13: rapid blink to alert user, then library auto-retries
    ledState = LED_RAPID_BLINK;
    ledRapidStart = millis();
    connectingPhase = true;
    connectStartedAt = millis();
    macAttempts = 1;  // library retries by MAC automatically
    Serial.println("[Audio] stopped, reconnecting...");
  }
}

void startA2DP() {
  if (sourceStarted || targetDeviceName.isEmpty()) return;
  // v1.0.10: disable BT sleep before starting — maximize radio availability
  esp_bt_sleep_disable();
  a2dp_source.set_auto_reconnect(true);  // library caches MAC in NVS, reconnects by MAC (no scan)
  // v1.0.13: don't override headset volume — let headset use its own remembered level
  a2dp_source.set_on_connection_state_changed(connectionStateChanged);
  a2dp_source.set_on_audio_state_changed(audioStateChanged);
  a2dp_source.set_data_callback_in_frames(getDataFrames);
  Serial.printf("[A2DP] starting -> %s\n", targetDeviceName.c_str());
  a2dp_source.start(targetDeviceName.c_str());
  sourceStarted = true;
  lastBtState   = "connecting";
  connectingPhase = true;
  connectStartedAt = millis();
  macAttempts = 1;  // first attempt is MAC-based (library auto-reconnect)

  // v1.0.12: enable ADC immediately after BT stack init — pre-fill ring buffer
  // so audio starts instantly when connection completes (no empty-ring silence gap)
  enableADC();
  Serial.println("[ADC] pre-filling ring buffer while BT connects");
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
  page += F("</div>");

  page += F("<div class='card'><h3>3) Input Gain</h3>");
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

// ---- LED control ----
void setupLED() {
  ledcAttach(kLedPin, kLedFreq, kLedRes);
  ledcWrite(kLedPin, 0);
}

void updateLED() {
  uint32_t ms = millis();

  // Determine state from flags
  if (btConnected) {
    ledState = LED_SOLID;
  } else if (ledState == LED_RAPID_BLINK && (ms - ledRapidStart < 3000)) {
    // Stay in rapid blink for 3 seconds (disconnect alert or final failure)
  } else if (connectingPhase) {
    ledState = LED_SLOW_BLINK;  // reconnecting
  } else if (wifiFallbackDone || !bootHasSavedDevice) {
    ledState = LED_BREATHING;   // AP mode
  } else {
    ledState = LED_OFF;
  }

  // LOLIN32 LED is active LOW: 0=full brightness, 255=off
  switch (ledState) {
    case LED_SOLID:
      ledcWrite(kLedPin, 0);  // fully on
      break;
    case LED_BREATHING: {
      // Smooth sine-wave breathing: ~3 second cycle
      float phase = (float)(ms % 3000) / 3000.0f * 2.0f * 3.14159f;
      uint8_t brightness = (uint8_t)((sinf(phase) + 1.0f) * 0.5f * 255.0f);
      ledcWrite(kLedPin, 255 - brightness);  // invert for active LOW
      break;
    }
    case LED_SLOW_BLINK: {
      // ~1Hz blink: 500ms on, 500ms off
      ledcWrite(kLedPin, (ms % 1000 < 500) ? 0 : 255);
      break;
    }
    case LED_RAPID_BLINK: {
      // 5Hz blink: 100ms on, 100ms off
      ledcWrite(kLedPin, (ms % 200 < 100) ? 0 : 255);
      break;
    }
    case LED_OFF:
    default:
      ledcWrite(kLedPin, 255);  // off
      break;
  }
}

// ---- Setup functions ----
void setupADC() {
  // v1.0.14: adc_continuous API — replaces legacy I2S ADC built-in mode
  // No more SYSCON register hacks — the API natively supports multi-channel scan

  // 1) Create handle
  adc_continuous_handle_cfg_t hdl_cfg = {};
  hdl_cfg.max_store_buf_size = 4096;
  hdl_cfg.conv_frame_size    = 512;  // bytes per conversion frame
  ESP_ERROR_CHECK(adc_continuous_new_handle(&hdl_cfg, &adcHandle));

  // 2) Configure scan pattern: CH6(L) → CH7(R) alternating
  adc_digi_pattern_config_t pattern[2] = {};
  pattern[0].atten     = ADC_ATTEN_DB_12;    // 0–3.3V range (was DB_11, now DB_12 in new API)
  pattern[0].channel   = kAdcChannelL;        // CH6 = GPIO34
  pattern[0].unit      = ADC_UNIT_1;
  pattern[0].bit_width = ADC_BITWIDTH_12;
  pattern[1].atten     = ADC_ATTEN_DB_12;
  pattern[1].channel   = kAdcChannelR;        // CH7 = GPIO35
  pattern[1].unit      = ADC_UNIT_1;
  pattern[1].bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t dig_cfg = {};
  dig_cfg.pattern_num    = 2;
  dig_cfg.adc_pattern    = pattern;
  dig_cfg.sample_freq_hz = kAdcSampleRate;
  dig_cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
  dig_cfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
  ESP_ERROR_CHECK(adc_continuous_config(adcHandle, &dig_cfg));

  // 3) Setup eFuse calibration (new API)
  adc_cali_line_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id  = ADC_UNIT_1;
  cali_cfg.atten    = ADC_ATTEN_DB_12;
  cali_cfg.bitwidth = ADC_BITWIDTH_12;
  esp_err_t cali_err = adc_cali_create_scheme_line_fitting(&cali_cfg, &adcCaliHandle);
  if (cali_err != ESP_OK) {
    Serial.printf("[ADC] calibration init failed: %s (will use raw values)\n", esp_err_to_name(cali_err));
    adcCaliHandle = NULL;
  } else {
    Serial.println("[ADC] eFuse calibration loaded");
  }

  // adc_continuous_start() is called later from enableADC()
  Serial.println("[ADC] continuous mode configured (CH6=L, CH7=R)");

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

  setupLED();   // PWM LED for status indication
  setupADC();   // I2S driver + SYSCON stereo pattern + ADC task on Core 1

  // v1.0.12: skip WiFi on boot if we have a saved device — saves heap for BT
  // WiFi starts later if BT connection fails (fallback)
  bootHasSavedDevice = !targetDeviceName.isEmpty();
  if (bootHasSavedDevice) {
    Serial.printf("[Boot] saved device '%s' — skipping WiFi, going straight to BT\n",
                  targetDeviceName.c_str());
    startA2DP();
  } else {
    setupWeb();
    Serial.println("http://192.168.4.1  (ESP32-Audio-Setup / esp32audio)");
  }
}

void loop() {
  // Only service web if WiFi is up
  if (!bootHasSavedDevice || wifiFallbackDone) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  // v1.0.12: connection retry logic
  // Library handles the actual BT connection/heartbeat internally.
  // We monitor from here and track attempt counts for the MAC→name→WiFi fallback.
  if (connectingPhase && !btConnected && sourceStarted) {
    unsigned long elapsed = millis() - connectStartedAt;

    // Check if current attempt has timed out
    if (elapsed > kConnectTimeout) {
      int totalMacAttempts = macAttempts;
      int totalNameAttempts = nameAttempts;

      if (totalMacAttempts < kMaxMacAttempts && totalNameAttempts == 0) {
        // Still in MAC phase — library heartbeat will auto-retry
        macAttempts++;
        connectStartedAt = millis();
        Serial.printf("[BT] MAC attempt %d/%d timed out, library retrying...\n",
                      totalMacAttempts, kMaxMacAttempts);
      } else if (totalMacAttempts >= kMaxMacAttempts && totalNameAttempts < kMaxNameAttempts) {
        // MAC phase exhausted — switch to name scan
        nameAttempts++;
        connectStartedAt = millis();
        Serial.printf("[BT] switching to name scan, attempt %d/%d\n",
                      nameAttempts, kMaxNameAttempts);
        // Disconnect and restart with name-based discovery
        a2dp_source.disconnect();
        delay(500);
        // Clear library's cached MAC so it falls through to name scan
        a2dp_source.clean_last_connection();
        a2dp_source.reconnect();
      } else {
        // All attempts exhausted — rapid blink for 3s, then start WiFi
        connectingPhase = false;
        ledState = LED_RAPID_BLINK;
        ledRapidStart = millis();
        Serial.println("[BT] all connection attempts failed — starting WiFi AP");
        if (!wifiFallbackDone) {
          setupWeb();
          wifiFallbackDone = true;
          Serial.println("http://192.168.4.1  (ESP32-Audio-Setup / esp32audio)");
        }
      }
    }
  }

  updateLED();
  delay(10);
}
