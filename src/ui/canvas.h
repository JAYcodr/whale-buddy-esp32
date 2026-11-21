#pragma once
#include <Arduino_GFX_Library.h>
#include <stdint.h>

#ifdef NOVA_HOST_SIM
constexpr int UI_W          = 368;
constexpr int UI_H          = 448;
constexpr int UI_SAFE_INSET = 16;
#else
#include "hw/display.h"
constexpr int UI_W          = HW_W;
constexpr int UI_H          = HW_H;
constexpr int UI_SAFE_INSET = SAFE_INSET;
#endif

constexpr int UI_SAFE_L = UI_SAFE_INSET;
constexpr int UI_SAFE_T = UI_SAFE_INSET;
constexpr int UI_SAFE_R = UI_W - UI_SAFE_INSET;
constexpr int UI_SAFE_B = UI_H - UI_SAFE_INSET;
constexpr int UI_SAFE_W = UI_W - 2 * UI_SAFE_INSET;
constexpr int UI_SAFE_H = UI_H - 2 * UI_SAFE_INSET;

using NovaCanvas = Arduino_Canvas;

// Helper for RGB565 literals
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
