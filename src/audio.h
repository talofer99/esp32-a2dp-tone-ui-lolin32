#pragma once
#include "config.h"

void setupADC();
void enableADC();
int32_t getDataFrames(Frame *frame, int32_t frame_count);
void audioStateChanged(esp_a2d_audio_state_t state, void *ptr);
