// src/hw/display.h
#pragma once
#include <Arduino_GFX_Library.h>
#include "hw/pins.h"   // provides BOARD_HW_W, BOARD_HW_H, BOARD_SAFE_INSET

constexpr int HW_W       = BOARD_HW_W;
constexpr int HW_H       = BOARD_HW_H;

// Logical-canvas safe-draw region for the rounded/circular AMOLED bezel.
constexpr int SAFE_INSET = BOARD_SAFE_INSET;
constexpr int SAFE_L     = SAFE_INSET;
constexpr int SAFE_T     = SAFE_INSET;
constexpr int SAFE_R     = HW_W - SAFE_INSET;
constexpr int SAFE_B     = HW_H - SAFE_INSET;
constexpr int SAFE_W     = HW_W - 2 * SAFE_INSET;
constexpr int SAFE_H     = HW_H - 2 * SAFE_INSET;

bool hwDisplayInit();
void hwDisplayPush();
void hwDisplayPushRect(int16_t x, int16_t y, int16_t w, int16_t h);
// 背光百分比 20..100（20 = 老固件的最暗档 ≈ 50/255，100 = 满亮 255）。越界按最近端点处理。
void hwDisplayBrightness(uint8_t pct);
void hwDisplaySleep(bool off);
Arduino_Canvas* hwCanvas();
