#pragma once
#include "config.h"

// Single interleaved stereo ring buffer: L, R, L, R, ...
// Core 1 (adcTask) writes, Core 0 (BT callback) reads.
extern int16_t  ringBuf[RING_SIZE];
extern volatile int ringWrite;
extern volatile int ringRead;

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

static inline void ringFlush() {
  ringWrite = 0;
  ringRead  = 0;
}
