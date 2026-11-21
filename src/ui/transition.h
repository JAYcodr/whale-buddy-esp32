#pragma once
#include <Arduino_GFX_Library.h>
#include <stdint.h>

enum class TransKind : uint8_t {
  SLIDE_LEFT,
  SLIDE_RIGHT,
  SHEET_UP,
  SHEET_DOWN,
  FADE,
};

bool transBuffers(uint16_t** from, uint16_t** to);
void transRun(Arduino_Canvas& canvas, const uint16_t* from, const uint16_t* to,
              TransKind kind, uint32_t ms);
