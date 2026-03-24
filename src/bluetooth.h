#pragma once
#include "config.h"

void startA2DP();
void connectionStateChanged(esp_a2d_connection_state_t state, void *ptr);
