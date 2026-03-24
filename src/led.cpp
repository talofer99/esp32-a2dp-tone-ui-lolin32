#include "led.h"

void setupLED() {
  ledcAttach(kLedPin, kLedFreq, kLedRes);
  ledcWrite(kLedPin, 255);  // off (active LOW)
}

void updateLED() {
  uint32_t ms = millis();

  if (btConnected) {
    ledState = LED_SOLID;
  } else if (ledState == LED_RAPID_BLINK && (ms - ledRapidStart < 3000)) {
    // Stay in rapid blink for 3 seconds
  } else if (connectingPhase) {
    ledState = LED_SLOW_BLINK;
  } else if (wifiFallbackDone || !bootHasSavedDevice) {
    ledState = LED_BREATHING;
  } else {
    ledState = LED_OFF;
  }

  // LOLIN32 LED is active LOW: 0=full brightness, 255=off
  switch (ledState) {
    case LED_SOLID:
      ledcWrite(kLedPin, 0);
      break;
    case LED_BREATHING: {
      float phase = (float)(ms % 3000) / 3000.0f * 2.0f * 3.14159f;
      uint8_t brightness = (uint8_t)((sinf(phase) + 1.0f) * 0.5f * 255.0f);
      ledcWrite(kLedPin, 255 - brightness);
      break;
    }
    case LED_SLOW_BLINK:
      ledcWrite(kLedPin, (ms % 1000 < 500) ? 0 : 255);
      break;
    case LED_RAPID_BLINK:
      ledcWrite(kLedPin, (ms % 200 < 100) ? 0 : 255);
      break;
    case LED_OFF:
    default:
      ledcWrite(kLedPin, 255);
      break;
  }
}
