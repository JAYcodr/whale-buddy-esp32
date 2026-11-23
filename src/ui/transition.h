#pragma once
#include <Arduino_GFX_Library.h>
#include <stdint.h>

enum class TransKind : uint8_t {
  SLIDE_LEFT,
  SLIDE_RIGHT,
  SHEET_UP,
  SHEET_DOWN,
  FADE,
  POP_IN,   // 目标帧从下方 kPopRisePx 升起 + 淡入（模态出现）
  POP_OUT,  // 源帧向下沉 kPopSinkPx + 淡出（模态消失）
};

bool transBuffers(uint16_t** from, uint16_t** to);
void transRun(Arduino_Canvas& canvas, const uint16_t* from, const uint16_t* to,
              TransKind kind, uint32_t ms);
