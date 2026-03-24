#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <BluetoothA2DPSource.h>
#include <hal/adc_types.h>

// ---- Firmware version ----
static const char *kFwVersion = "1.0.15";

// Uncomment to enable periodic ADC/A2DP stats on serial (every 2s)
// #define DEBUG_STATS

// ---- Constants ----
static const char *kApSsid     = "ESP32-Audio-Setup";
static const char *kApPassword = "esp32audio";
static const uint32_t kSampleRate = 44100;
// adc_continuous sample rate: total conversions/sec across both channels
// ESP32 ADC has ~82% efficiency. 53878 configured → ~44,160 actual → ~22,080/sec per channel
static const uint32_t kAdcSampleRate = 53878;
static const int kAdcChannelL = ADC_CHANNEL_6;  // GPIO34 — left
static const int kAdcChannelR = ADC_CHANNEL_7;  // GPIO35 — right

// ---- LED ----
static const int kLedPin  = 5;       // GPIO5 = LED_BUILTIN on LOLIN32
static const int kLedFreq = 5000;
static const int kLedRes  = 8;       // 8-bit PWM (0-255)

enum LedState { LED_OFF, LED_BREATHING, LED_SLOW_BLINK, LED_SOLID, LED_RAPID_BLINK };

// ---- Reconnect ----
static const int kMaxMacAttempts  = 3;
static const int kMaxNameAttempts = 2;
static const unsigned long kConnectTimeout = 15000;

// ---- Ring buffer ----
#define RING_SIZE 4096

// ---- ADC ----
static const int32_t kMidpointMv = 1650;  // DC bias: 2×100K divider on 3.3V

// ---- Shared state (extern — defined in main.cpp) ----
extern BluetoothA2DPSource a2dp_source;
extern WebServer  server;
extern DNSServer  dnsServer;
extern Preferences prefs;

extern String targetDeviceName;
extern bool   sourceStarted;
extern bool   toneEnabled;
extern volatile bool btConnected;
extern String lastBtState;
extern volatile int audioVolume;
extern bool   adcEnabled;

extern int  macAttempts;
extern int  nameAttempts;
extern bool connectingPhase;
extern unsigned long connectStartedAt;
extern bool wifiFallbackDone;
extern bool bootHasSavedDevice;

extern LedState ledState;
extern unsigned long ledRapidStart;
extern volatile bool resetResampler;
