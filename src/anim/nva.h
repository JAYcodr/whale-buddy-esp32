#pragma once
#include "ui/canvas.h"
#include <stdint.h>

enum class NvaAnimKey : uint8_t {
  IDLE = 0,
  THINKING,
  WORKING,
  WAITING,
  SUCCESS,
  ERROR,
  CLICK,
  SLEEP,
  QUOTA,
  COUNT
};

bool nvaInit(const char* rootDir = "/pets/nova");
bool nvaAvailable();
void nvaSetState(NvaAnimKey key);
NvaAnimKey nvaCurrentState();
void nvaTick(uint32_t nowMs);

// Blit current decoded frame centered inside (cx, bottomY) onto canvas.
// Returns true if a frame was drawn.
bool nvaDraw(NovaCanvas& c, int16_t centerX, int16_t bottomY);
