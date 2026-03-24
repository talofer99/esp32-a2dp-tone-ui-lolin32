#include "config.h"
#include "audio.h"
#include "bluetooth.h"
#include "web_ui.h"
#include "led.h"

// ---- Shared state definitions (declared extern in config.h) ----
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
bool   adcEnabled       = false;

int  macAttempts     = 0;
int  nameAttempts    = 0;
bool connectingPhase = false;
unsigned long connectStartedAt = 0;
bool wifiFallbackDone = false;
bool bootHasSavedDevice = false;

LedState ledState = LED_OFF;
unsigned long ledRapidStart = 0;
volatile bool resetResampler = false;

// ---- Arduino entry points ----
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n=== ESP32 A2DP Audio v%s ===\n", kFwVersion);

  prefs.begin("audio", false);
  targetDeviceName = prefs.getString("device", "");
  audioVolume      = prefs.getInt("volume", 80);
  prefs.end();

  if (targetDeviceName.length()) {
    Serial.printf("[Prefs] last device: %s  volume: %d\n",
                  targetDeviceName.c_str(), (int)audioVolume);
  }

  setupLED();
  setupADC();

  bootHasSavedDevice = !targetDeviceName.isEmpty();
  if (bootHasSavedDevice) {
    Serial.printf("[Boot] saved device '%s' — skipping WiFi, going straight to BT\n",
                  targetDeviceName.c_str());
    startA2DP();
  } else {
    setupWeb();
  }
}

void loop() {
  if (!bootHasSavedDevice || wifiFallbackDone) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  // Connection retry logic
  if (connectingPhase && !btConnected && sourceStarted) {
    unsigned long elapsed = millis() - connectStartedAt;

    if (elapsed > kConnectTimeout) {
      int totalMacAttempts = macAttempts;
      int totalNameAttempts = nameAttempts;

      if (totalMacAttempts < kMaxMacAttempts && totalNameAttempts == 0) {
        macAttempts++;
        connectStartedAt = millis();
        Serial.printf("[BT] MAC attempt %d/%d timed out, library retrying...\n",
                      totalMacAttempts, kMaxMacAttempts);
      } else if (totalMacAttempts >= kMaxMacAttempts && totalNameAttempts < kMaxNameAttempts) {
        nameAttempts++;
        connectStartedAt = millis();
        Serial.printf("[BT] switching to name scan, attempt %d/%d\n",
                      nameAttempts, kMaxNameAttempts);
        a2dp_source.disconnect();
        delay(500);
        a2dp_source.clean_last_connection();
        a2dp_source.reconnect();
      } else {
        connectingPhase = false;
        ledState = LED_RAPID_BLINK;
        ledRapidStart = millis();
        Serial.println("[BT] all connection attempts failed — starting WiFi AP");
        if (!wifiFallbackDone) {
          setupWeb();
          wifiFallbackDone = true;
        }
      }
    }
  }

  updateLED();
  delay(10);
}
