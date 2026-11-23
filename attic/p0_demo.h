#pragma once
#include "ui/canvas.h"
#include <stdint.h>

struct P0DemoState {
  uint32_t frame;
  float    fps;
  uint32_t pushUs;
  uint32_t freeHeap;
  uint32_t freePsram;
  uint16_t battMv;
  uint8_t  battPct;
  bool     usbPresent;
  bool     touchDown;
  int16_t  touchX;
  int16_t  touchY;
  uint8_t  hour;
  uint8_t  minute;
  uint8_t  second;
};

struct DirtyRect {
  int16_t x, y, w, h;
};

void uiDrawP0Full(NovaCanvas& c, const P0DemoState& st);
DirtyRect uiDrawP0AnimRegion(NovaCanvas& c, const P0DemoState& st);
