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
void nvaSetState(NvaAnimKey key, bool once = false, bool restart = false);
NvaAnimKey nvaCurrentState();
NvaAnimKey nvaActiveState();
bool nvaIsEnded();
uint32_t nvaLoopCycles();
void nvaTick(uint32_t nowMs);
uint32_t nvaDecodeBusyUs();  // 累计解码耗时（us），供 UI 任务折算 decBusyPct
int16_t nvaFrameWidth();
int16_t nvaFrameHeight();

// Blit current decoded frame centered inside (cx, bottomY) onto canvas.
// Supports optional horizontal mirror (flipX) and bottom-anchored Q-bounce scaleY.
// Returns true if a frame was drawn.
bool nvaDraw(NovaCanvas& c, int16_t centerX, int16_t bottomY, bool flipX = false, float scaleY = 1.0f);
