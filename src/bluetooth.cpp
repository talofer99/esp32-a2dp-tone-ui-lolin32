#include "bluetooth.h"
#include "audio.h"
#include "ring_buffer.h"
#include <esp_wifi.h>
#include <esp_gap_bt_api.h>

void connectionStateChanged(esp_a2d_connection_state_t state, void *ptr) {
  (void)ptr;
  lastBtState = a2dp_source.to_str(state);
  btConnected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  Serial.printf("[A2DP] state: %s\n", lastBtState.c_str());

  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    connectingPhase = false;
    macAttempts = 0;
    nameAttempts = 0;

    esp_wifi_stop();

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

    Serial.printf("[BT] connected to MAC: %s\n", a2dp_source.to_str(peer));

    ringFlush();
    resetResampler = true;

    toneEnabled = true;
    enableADC();
    Serial.println("[Audio] streaming enabled, WiFi stopped");
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    toneEnabled = false;
    btConnected = false;
    ledState = LED_RAPID_BLINK;
    ledRapidStart = millis();
    connectingPhase = true;
    connectStartedAt = millis();
    macAttempts = 1;
    Serial.println("[Audio] stopped, reconnecting...");
  }
}

void startA2DP() {
  if (sourceStarted || targetDeviceName.isEmpty()) return;
  esp_bt_sleep_disable();
  a2dp_source.set_auto_reconnect(true);
  a2dp_source.set_on_connection_state_changed(connectionStateChanged);
  a2dp_source.set_on_audio_state_changed(audioStateChanged);
  a2dp_source.set_data_callback_in_frames(getDataFrames);
  Serial.printf("[A2DP] starting -> %s\n", targetDeviceName.c_str());
  a2dp_source.start(targetDeviceName.c_str());
  sourceStarted = true;
  lastBtState   = "connecting";
  connectingPhase = true;
  connectStartedAt = millis();
  macAttempts = 1;

  enableADC();
  Serial.println("[ADC] pre-filling ring buffer while BT connects");
}
