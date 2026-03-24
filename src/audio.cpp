#include "audio.h"
#include "ring_buffer.h"
#include <esp_adc/adc_continuous.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

// Ring buffer storage (shared via ring_buffer.h externs)
int16_t  ringBuf[RING_SIZE];
volatile int ringWrite = 0;
volatile int ringRead  = 0;

// Private to this module
static adc_continuous_handle_t adcHandle = NULL;
static adc_cali_handle_t adcCaliHandle = NULL;
static volatile bool stereoActive = false;

// ---- ADC sampling task — Core 1 ----
static void adcTask(void *) {
  static uint8_t buf[512];  // 256 results × 2 bytes each
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
    static int16_t pendingL    = 0;
    static bool    hasPendingL = false;
    readCount++;
    for (int i = 0; i < n; i++) {
      uint8_t  chId = results[i].type1.channel;
      uint16_t raw  = results[i].type1.data;
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
        ringPush(pendingL);
        ringPush((int16_t)adc);
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
int32_t getDataFrames(Frame *frame, int32_t frame_count) {
  int vol = audioVolume;
  bool active = toneEnabled && btConnected;

  static int16_t rsPrevL = 0, rsCurrL = 0;
  static int16_t rsPrevR = 0, rsCurrR = 0;
  static float   rsPhase = 0.0f;
  static float   kRatio  = 0.515f;

  static uint32_t drainCount = 0;
  static uint32_t callCount  = 0;
  static uint32_t drainTick  = 0;

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
      while (rsPhase >= 1.0f) {
        if (ringAvail() >= 2) {
          rsPrevL = rsCurrL;  rsPrevR = rsCurrR;
          rsCurrL = ringPop();
          rsCurrR = ringPop();
        }
        rsPhase -= 1.0f;
      }
      int32_t sL = (int32_t)rsPrevL + (int32_t)(((int32_t)rsCurrL - rsPrevL) * rsPhase);
      int32_t sR = (int32_t)rsPrevR + (int32_t)(((int32_t)rsCurrR - rsPrevR) * rsPhase);
      frame[i].channel1 = (int16_t)(sL * 12 * vol / 100);
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
    uint32_t drainPerSec = drainCount / 2;
    if (drainPerSec > 1000) {
      float measured = 22080.0f / (float)drainPerSec;
      kRatio = 0.8f * measured + 0.2f * kRatio;
    }
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

// ---- Enable/disable ADC ----
void enableADC() {
  if (!adcEnabled && adcHandle != NULL) {
    esp_err_t err = adc_continuous_start(adcHandle);
    if (err == ESP_OK) {
      adcEnabled = true;
      ringFlush();
      Serial.println("[ADC] continuous mode started");
    } else {
      Serial.printf("[ADC] start failed: %s\n", esp_err_to_name(err));
    }
  }
}

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

// ---- Setup ----
void setupADC() {
  adc_continuous_handle_cfg_t hdl_cfg = {};
  hdl_cfg.max_store_buf_size = 4096;
  hdl_cfg.conv_frame_size    = 512;
  ESP_ERROR_CHECK(adc_continuous_new_handle(&hdl_cfg, &adcHandle));

  adc_digi_pattern_config_t pattern[2] = {};
  pattern[0].atten     = ADC_ATTEN_DB_12;
  pattern[0].channel   = kAdcChannelL;
  pattern[0].unit      = ADC_UNIT_1;
  pattern[0].bit_width = ADC_BITWIDTH_12;
  pattern[1].atten     = ADC_ATTEN_DB_12;
  pattern[1].channel   = kAdcChannelR;
  pattern[1].unit      = ADC_UNIT_1;
  pattern[1].bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t dig_cfg = {};
  dig_cfg.pattern_num    = 2;
  dig_cfg.adc_pattern    = pattern;
  dig_cfg.sample_freq_hz = kAdcSampleRate;
  dig_cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
  dig_cfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
  ESP_ERROR_CHECK(adc_continuous_config(adcHandle, &dig_cfg));

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

  Serial.println("[ADC] continuous mode configured (CH6=L, CH7=R)");
  xTaskCreatePinnedToCore(adcTask, "adc_task", 4096, NULL, 5, NULL, 1);
  Serial.println("[ADC] sampling task on Core 1");
}
