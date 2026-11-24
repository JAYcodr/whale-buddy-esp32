#pragma once
#include "hw/power.h"
#include "hw/rtc.h"
#include <stdint.h>

enum class InputEvtType : uint8_t {
  TOUCH_DOWN = 0,
  TOUCH_MOVE,
  TOUCH_UP,
  KEY1_DOWN,
  KEY1_UP,
  AXP_SHORT,
  AXP_LONG,
};

struct InputEvent {
  InputEvtType type;
  int16_t      x;
  int16_t      y;
  uint32_t     tsMs;
};

struct BusSnapshot {
  HwBattery battery;
  HwTime    time;
  bool      timeValid;
  float     ax, ay, az;
  bool      faceDown;
  bool      touchDown;
  int16_t   touchX, touchY;
  bool      key1Down;
};

bool hwBusStart();
BusSnapshot hwBusSnapshot();
bool hwBusPollInput(InputEvent* out);
bool hwBusRequestRtcWrite(const HwTime& t);
bool hwBusRequestMicGain(uint8_t gainDb);
uint32_t hwBusInputDropCount();   // 输入队列满导致的丢弃次数
