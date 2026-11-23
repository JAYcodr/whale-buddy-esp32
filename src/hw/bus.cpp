#include "hw/bus.h"
#include "hw/hw.h"
#include "log.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

static portMUX_TYPE   s_snapMux = portMUX_INITIALIZER_UNLOCKED;
static BusSnapshot    s_snap    = {};
static QueueHandle_t  s_inputQ  = nullptr;
static QueueHandle_t  s_rtcQ    = nullptr;
static TaskHandle_t   s_busTask = nullptr;
static uint32_t       s_inputDrops = 0;

static void pushEvt(InputEvtType type, int16_t x, int16_t y, uint32_t now) {
  if (!s_inputQ) return;
  InputEvent e{ type, x, y, now };
  if (xQueueSend(s_inputQ, &e, 0) != pdTRUE) {
    s_inputDrops++;
    NOVA_LOG(W, "bus", "input queue full, drop evt=%u\n", (unsigned)type);
  }
}

static void busTaskFn(void*) {
  esp_task_wdt_add(nullptr);
  uint32_t tickCount = 0;
  int8_t faceDownFrames = 0;
  int16_t lastTx = -1, lastTy = -1;
  uint8_t i2cErrStreak = 0;

  // Initial read so snapshot is valid immediately after startup.
  // settingsLoad() has run by now, so this tick also applies the saved
  // charge-limit / charge-speed policy.
  hwPowerTick(millis());
  HwBattery bat = hwBattery();
  HwTime tm = {};
  bool tmOk = hwRtcRead(&tm);
  float ax = 0.0f, ay = 0.0f, az = 1.0f;
  hwImuAccel(&ax, &ay, &az);

  taskENTER_CRITICAL(&s_snapMux);
  s_snap.battery   = bat;
  s_snap.time      = tm;
  s_snap.timeValid = tmOk;
  s_snap.ax        = ax;
  s_snap.ay        = ay;
  s_snap.az        = az;
  taskEXIT_CRITICAL(&s_snapMux);

  for (;;) {
    uint32_t now = millis();

    // Handle pending RTC write from task_net without ever sharing Wire.
    HwTime pendingRtc;
    if (s_rtcQ && xQueueReceive(s_rtcQ, &pendingRtc, 0) == pdTRUE) {
      hwRtcWrite(pendingRtc);
      tmOk = hwRtcRead(&tm);
    }

    // 1. Poll input (touch, Key1, AXP PEK) every cycle (~20 ms or on touch IRQ).
    hwInputUpdate();
    const HwTouch& tp = hwTouch();
    const HwBtn& k1   = hwBtnA();
    uint8_t axp       = hwAxpBtnEvent();

    if (tp.justPressed) {
      pushEvt(InputEvtType::TOUCH_DOWN, tp.x, tp.y, now);
      lastTx = tp.x;
      lastTy = tp.y;
    } else if (tp.down && (tp.x != lastTx || tp.y != lastTy)) {
      pushEvt(InputEvtType::TOUCH_MOVE, tp.x, tp.y, now);
      lastTx = tp.x;
      lastTy = tp.y;
    } else if (tp.justReleased) {
      pushEvt(InputEvtType::TOUCH_UP, tp.x, tp.y, now);
      lastTx = -1;
      lastTy = -1;
    }

    if (k1.wasPressed)  pushEvt(InputEvtType::KEY1_DOWN, 0, 0, now);
    if (k1.wasReleased) pushEvt(InputEvtType::KEY1_UP, 0, 0, now);
    if (hwBtnB().wasPressed) pushEvt(InputEvtType::AXP_SHORT, 0, 0, now);
    if (axp == 0x04)         pushEvt(InputEvtType::AXP_LONG, 0, 0, now);

    // 2. IMU every cycle (debounced face-down detector).
    hwImuAccel(&ax, &ay, &az);
    bool down = (az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f);
    if (down) {
      if (faceDownFrames < 20) faceDownFrames++;
    } else {
      if (faceDownFrames > -10) faceDownFrames--;
    }
    bool faceDown = s_snap.faceDown;
    if (!faceDown && faceDownFrames >= 15) faceDown = true;
    else if (faceDown && faceDownFrames <= -8) faceDown = false;

    // 3. RTC every ~200 ms (10 ticks), Battery every ~1000 ms (50 ticks).
    // hwRtcRead() 失败时不更新 tm（保留上一份有效时间），并累计错误；
    // 连续 10 次（约 2 s）才重初始化 I2C，避免一次抖动就重置总线。
    if ((tickCount % 10) == 0) {
      HwTime nextTm = {};
      if (hwRtcRead(&nextTm)) {
        tm = nextTm;
        tmOk = true;
        i2cErrStreak = 0;
      } else if (++i2cErrStreak >= 10) {
        i2cErrStreak = 0;
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
        Wire.setClock(400000);
        Wire.setTimeOut(50);
      }
    }
    if ((tickCount % 50) == 0) {
      hwPowerTick(now);
      bat = hwBattery();
    }
    tickCount++;
    esp_task_wdt_reset();

    taskENTER_CRITICAL(&s_snapMux);
    s_snap.battery   = bat;
    s_snap.time      = tm;
    s_snap.timeValid = tmOk;
    s_snap.ax        = ax;
    s_snap.ay        = ay;
    s_snap.az        = az;
    s_snap.faceDown  = faceDown;
    s_snap.touchDown = tp.down;
    s_snap.touchX    = tp.x;
    s_snap.touchY    = tp.y;
    s_snap.key1Down  = k1.isPressed;
    taskEXIT_CRITICAL(&s_snapMux);

    // 20 ms period split into 4 ms slices so touch-down IRQ breaks early.
    for (int i = 0; i < 5; i++) {
      vTaskDelay(pdMS_TO_TICKS(4));
      if (hwTouchIrqPending()) break;
    }
  }
}

bool hwBusStart() {
  if (s_busTask) return true;
  s_inputQ = xQueueCreate(32, sizeof(InputEvent));
  s_rtcQ   = xQueueCreate(1, sizeof(HwTime));
  if (!s_inputQ || !s_rtcQ) return false;
  BaseType_t ok = xTaskCreatePinnedToCore(
      busTaskFn, "task_bus", 4096, nullptr, 6, &s_busTask, 0);
  return ok == pdPASS;
}

BusSnapshot hwBusSnapshot() {
  BusSnapshot copy;
  taskENTER_CRITICAL(&s_snapMux);
  copy = s_snap;
  taskEXIT_CRITICAL(&s_snapMux);
  return copy;
}

bool hwBusPollInput(InputEvent* out) {
  if (!s_inputQ || !out) return false;
  return xQueueReceive(s_inputQ, out, 0) == pdTRUE;
}

bool hwBusRequestRtcWrite(const HwTime& t) {
  if (!s_rtcQ) return false;
  return xQueueOverwrite(s_rtcQ, &t) == pdTRUE;
}

uint32_t hwBusInputDropCount() { return s_inputDrops; }
