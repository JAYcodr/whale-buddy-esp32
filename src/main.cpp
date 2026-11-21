#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "anim/nva.h"
#include "audio/sfx.h"
#include "hw/bus.h"
#include "hw/hw.h"
#include "model/settings.h"
#include "model/state.h"
#include "net/net.h"
#include "ui/canvas.h"
#include "ui/nav.h"
#include "ui/pages.h"

namespace {

constexpr const char* FW_VER = "0.3.0";
constexpr uint32_t SCREEN_OFF_BAT_MS = 30000;
constexpr uint32_t KEY1_LONG_MS = 800;
constexpr uint32_t PAGE_SLIDE_MS = 180;
constexpr int16_t  SLIDE_SPLIT_Y = 46;  // Keep top clock/status steady; slide pills + page content.

uint16_t* s_slideFrom = nullptr;
uint16_t* s_slideTo   = nullptr;

void runHorizontalSlide(Arduino_Canvas& canvas, UiNav& nav, const NovaState& st,
                        const NovaSettings& cfg, uint32_t frame, uint32_t nowMs, int slideDir) {
  uint16_t* fb = canvas.getFramebuffer();
  if (!fb) {
    nav.draw(canvas, st, cfg, frame, nowMs);
    hwDisplayPush();
    return;
  }

  const size_t pxCount = (size_t)UI_W * UI_H;
  const size_t fbBytes = pxCount * sizeof(uint16_t);
  if (!s_slideFrom) s_slideFrom = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_slideTo)   s_slideTo   = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!s_slideFrom || !s_slideTo) {
    nav.draw(canvas, st, cfg, frame, nowMs);
    hwDisplayPush();
    return;
  }

  memcpy(s_slideFrom, fb, fbBytes);
  nav.draw(canvas, st, cfg, frame, nowMs);
  memcpy(s_slideTo, fb, fbBytes);

  // Header top band stays fixed from destination frame.
  memcpy(fb, s_slideTo, (size_t)SLIDE_SPLIT_Y * UI_W * sizeof(uint16_t));

  const int64_t tStartUs = esp_timer_get_time();
  const int64_t durUs = (int64_t)PAGE_SLIDE_MS * 1000LL;
  while (true) {
    int64_t elapsedUs = esp_timer_get_time() - tStartUs;
    if (elapsedUs >= durUs) break;
    float u = (float)elapsedUs / (float)durUs;
    float inv = 1.0f - u;
    float ease = 1.0f - inv * inv * inv;
    int offset = (int)(ease * UI_W + 0.5f);
    if (offset < 0) offset = 0;
    if (offset > UI_W) offset = UI_W;

    int keepFrom = UI_W - offset;
    for (int y = SLIDE_SPLIT_Y; y < UI_H; y++) {
      uint16_t* dstRow = fb + (size_t)y * UI_W;
      const uint16_t* srcFrom = s_slideFrom + (size_t)y * UI_W;
      const uint16_t* srcTo   = s_slideTo   + (size_t)y * UI_W;
      if (slideDir > 0) {
        if (keepFrom > 0) memcpy(dstRow, srcFrom + offset, (size_t)keepFrom * sizeof(uint16_t));
        if (offset > 0)   memcpy(dstRow + keepFrom, srcTo, (size_t)offset * sizeof(uint16_t));
      } else {
        if (offset > 0)   memcpy(dstRow, srcTo + keepFrom, (size_t)offset * sizeof(uint16_t));
        if (keepFrom > 0) memcpy(dstRow + offset, srcFrom, (size_t)keepFrom * sizeof(uint16_t));
      }
    }
    hwDisplayPush();
  }

  memcpy(fb, s_slideTo, fbBytes);
  hwDisplayPush();
}

void sendUsbScreenshot() {
  Arduino_Canvas* c = hwCanvas();
  if (!c) return;
  const uint16_t* fb = c->getFramebuffer();
  if (!fb) return;

  const uint16_t w = (uint16_t)HW_W;
  const uint16_t h = (uint16_t)HW_H;
  const size_t bytes = (size_t)w * h * sizeof(uint16_t);
  uint8_t hdr[12] = {
    'N', 'V', 'S', 'H',
    (uint8_t)(w & 0xFF), (uint8_t)(w >> 8),
    (uint8_t)(h & 0xFF), (uint8_t)(h >> 8),
    (uint8_t)(bytes & 0xFF),
    (uint8_t)((bytes >> 8) & 0xFF),
    (uint8_t)((bytes >> 16) & 0xFF),
    (uint8_t)((bytes >> 24) & 0xFF),
  };
  Serial.write(hdr, sizeof(hdr));
  const uint8_t* ptr = (const uint8_t*)fb;
  size_t sent = 0;
  while (sent < bytes) {
    size_t chunk = bytes - sent;
    if (chunk > 4096) chunk = 4096;
    size_t wlen = Serial.write(ptr + sent, chunk);
    if (wlen == 0) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
    sent += wlen;
  }
  Serial.flush();
}

void pollSerialCommands() {
  static char line[192];
  static size_t pos = 0;
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      line[pos] = '\0';
      if (line[0] == '{') {
        JsonDocument doc;
        if (deserializeJson(doc, line) == DeserializationError::Ok) {
          const char* cmd = doc["cmd"] | "";
          if (strcmp(cmd, "shot") == 0) {
            sendUsbScreenshot();
          } else if (strcmp(cmd, "reprov") == 0) {
            netRequestReprovision();
          } else if (strcmp(cmd, "wifi") == 0) {
            const char* ssid = doc["ssid"] | "";
            const char* pass = doc["pass"] | "";
            if (ssid[0]) netRequestWifiConnect(ssid, pass);
          }
        }
      } else if (strstr(line, "shot") != nullptr) {
        sendUsbScreenshot();
      } else if (strstr(line, "reprov") != nullptr) {
        netRequestReprovision();
      }
      pos = 0;
    } else if (pos + 1 < sizeof(line)) {
      line[pos++] = ch;
    }
  }
}

void uiTaskFn(void*) {
  Arduino_Canvas* canvas = hwCanvas();
  UiNav nav;
  nav.init(settingsGet().homePage);

  bool screenOff = false;
  bool dimmedForNap = false;
  bool needFull = true;

  int16_t tpStartX = 0, tpStartY = 0, tpLastY = 0;
  bool    tpActive = false, tpDraggedV = false;
  uint32_t key1DownAt = 0;
  bool     key1LongFired = false;
  uint32_t lastInteractMs = millis();
  uint32_t clickAnimUntilMs = 0;

  uint32_t lastApGen = 0;
  uint32_t lastStateGen = 0;
  uint32_t lastQuotaGen = 0;
  uint32_t lastRankGen = 0;
  bool     lastWifiUp = false;
  bool     lastApMode = false;
  bool     lastMqttUp = false;
  uint8_t  lastMinute = 255;
  uint8_t  lastSecond = 255;
  uint8_t  lastBattPct = 255;

  uint32_t frame = 0;
  uint32_t fpsFrames = 0;
  int64_t  fpsWindowStartUs = esp_timer_get_time();
  float    curFps = 0.0f;
  uint32_t lastPushUs = 0;
  int64_t  nextFrameAt = esp_timer_get_time();

  for (;;) {
    uint32_t nowMs = millis();
    pollSerialCommands();

    const NovaSettings& cfg = settingsGet();
    BusSnapshot snap = hwBusSnapshot();

    // Update shared NovaState hardware fields.
    novaStateWithLock([&](NovaState& st) {
      st.fps        = curFps;
      st.pushUs     = lastPushUs;
      st.freeHeap   = esp_get_free_heap_size();
      st.freePsram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      st.battMv     = snap.battery.mV;
      st.battPct    = snap.battery.pct;
      st.usbPresent = snap.battery.usbPresent;
      st.charging   = snap.battery.charging;
      st.faceDown   = snap.faceDown;
      st.hour       = snap.time.H;
      st.minute     = snap.time.M;
      st.second     = snap.time.S;
      st.timeValid  = snap.timeValid;

      // Auto-expire approval on device side after TTL.
      if (st.approvalActive && st.apTtlMs > 0 && (nowMs - st.apArrivedMs >= st.apTtlMs)) {
        st.approvalActive = false;
        st.apGen++;
      }
    });

    static NovaState st;
    novaStateRead(&st);

    // Handle new approval arrival: wake idle screen (unless face-down napping).
    if (st.apGen != lastApGen) {
      lastApGen = st.apGen;
      needFull = true;
      if (st.approvalActive) {
        if (!snap.faceDown && screenOff) {
          hwDisplaySleep(false);
          hwDisplayBrightness(cfg.brightLevel);
          screenOff = false;
        }
        if (!snap.faceDown) {
          sfxPlay(SfxKind::APPROVAL_ARRIVE, cfg.soundPct);
          lastInteractMs = nowMs;
        }
      }
    }

    // Raise WiFi power mode during approval or active interaction.
    bool needLowLatencyNet = st.approvalActive || st.wifiApMode || ((nowMs - lastInteractMs) < 5000);
    netSetPowerSave(needLowLatencyNet ? NetPsMode::PERFORMANCE : NetPsMode::LOW_POWER);

    // Process discrete input events from task_bus.
    auto wakeScreen = [&]() {
      lastInteractMs = nowMs;
      if (screenOff) {
        hwDisplaySleep(false);
        hwDisplayBrightness(cfg.brightLevel);
        screenOff = false;
        needFull = true;
      }
      if (dimmedForNap) {
        hwDisplayBrightness(cfg.brightLevel);
        dimmedForNap = false;
      }
    };

    auto handleAction = [&](const UiAction& act, int slideDir = 0) {
      if (act.kind == UiAction::NONE) return;
      if (act.kind != UiAction::PET_TAP) needFull = true;
      switch (act.kind) {
        case UiAction::GO_PAGE:
          nav.setPage((UiPage)act.value);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          if (slideDir != 0 && canvas && !screenOff && !dimmedForNap) {
            runHorizontalSlide(*canvas, nav, st, cfg, frame, nowMs, slideDir);
            needFull = false;
            frame++;
            fpsFrames++;
          }
          break;
        case UiAction::SET_HOME:
          settingsMut().homePage = (HomePageKind)act.value;
          settingsSave();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_POWER:
          settingsMut().powerProfile = (PowerProfile)act.value;
          settingsSave();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::TOGGLE_SOUND:
          settingsMut().soundPct = (cfg.soundPct > 0) ? 0 : 70;
          settingsSave();
          if (settingsGet().soundPct > 0) sfxPlay(SfxKind::NAV_CHIRP, settingsGet().soundPct);
          break;
        case UiAction::SET_BRIGHT:
          settingsMut().brightLevel = (uint8_t)act.value;
          settingsSave();
          hwDisplayBrightness(settingsGet().brightLevel);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::REPROVISION:
          netRequestReprovision();
          nav.setPage(UiPage::HOME);
          sfxPlay(SfxKind::NEW_EVENT, cfg.soundPct);
          break;
        case UiAction::CANCEL_PORTAL:
          netRequestCancelPortal();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::PET_TAP:
          clickAnimUntilMs = nowMs + 2600;
          nvaSetState(NvaAnimKey::CLICK);
          sfxPlay(SfxKind::NEW_EVENT, cfg.soundPct);
          break;
        case UiAction::APPROVE:
          netPublishDecision(st.apId, true, false);
          novaStateWithLock([](NovaState& s) { s.approvalActive = false; s.apGen++; });
          sfxPlay(SfxKind::ACCEPT, cfg.soundPct);
          break;
        case UiAction::DENY:
          netPublishDecision(st.apId, false, false);
          novaStateWithLock([](NovaState& s) { s.approvalActive = false; s.apGen++; });
          sfxPlay(SfxKind::DENY, cfg.soundPct);
          break;
        case UiAction::TO_PC:
          netPublishDecision(st.apId, false, true);
          novaStateWithLock([](NovaState& s) { s.approvalActive = false; s.apGen++; });
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::RANK_REDRAW:
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        default: break;
      }
    };

    InputEvent ev;
    while (hwBusPollInput(&ev)) {
      if (ev.type == InputEvtType::TOUCH_DOWN) {
        if (screenOff) {
          wakeScreen();
          tpActive = false;
          continue;
        }
        wakeScreen();
        tpActive = true;
        tpDraggedV = false;
        tpStartX = ev.x;
        tpStartY = ev.y;
        tpLastY  = ev.y;
      } else if (ev.type == InputEvtType::TOUCH_MOVE && tpActive) {
        wakeScreen();
        if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
          int16_t adx = ev.x > tpStartX ? (ev.x - tpStartX) : (tpStartX - ev.x);
          int16_t ady = ev.y > tpStartY ? (ev.y - tpStartY) : (tpStartY - ev.y);
          if (!tpDraggedV && ady >= 12 && ady > adx) {
            tpDraggedV = true;
          }
          if (tpDraggedV) {
            int16_t stepY = ev.y - tpLastY;
            if (stepY != 0 && pageRankScrollPx(stepY, st.nRows)) {
              needFull = true;
            }
          }
        }
        tpLastY = ev.y;
      } else if (ev.type == InputEvtType::TOUCH_UP && tpActive) {
        tpActive = false;
        wakeScreen();
        if (tpDraggedV) {
          tpDraggedV = false;
          continue;
        }
        int16_t dx = ev.x - tpStartX;
        int16_t dy = ev.y - tpStartY;
        UiAction act = st.approvalActive ? UiAction{} : nav.onSwipe(dx, dy);
        if (act.kind != UiAction::NONE) {
          handleAction(act, dx < 0 ? 1 : -1);
        } else {
          act = nav.onTap(ev.x, ev.y, st, cfg);
          handleAction(act);
        }
      } else if (ev.type == InputEvtType::KEY1_DOWN) {
        wakeScreen();
        key1DownAt = nowMs;
        key1LongFired = false;
      } else if (ev.type == InputEvtType::KEY1_UP) {
        if (!key1LongFired && key1DownAt != 0) {
          nav.goHome(cfg.homePage);
          needFull = true;
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
        }
        key1DownAt = 0;
      } else if (ev.type == InputEvtType::AXP_SHORT) {
        if (screenOff) {
          wakeScreen();
        } else {
          hwDisplaySleep(true);
          screenOff = true;
        }
      }
    }

    if (snap.key1Down && key1DownAt != 0 && !key1LongFired && (nowMs - key1DownAt >= KEY1_LONG_MS)) {
      key1LongFired = true;
      nav.setPage(UiPage::SETTINGS);
      needFull = true;
      sfxPlay(SfxKind::NEW_EVENT, cfg.soundPct);
    }

    // Face-down nap dim/wake (skip during approval).
    if (!st.approvalActive && !screenOff) {
      if (snap.faceDown && !dimmedForNap) {
        dimmedForNap = true;
        hwDisplayBrightness(0);
      } else if (!snap.faceDown && dimmedForNap) {
        dimmedForNap = false;
        hwDisplayBrightness(cfg.brightLevel);
        needFull = true;
      }
    }

    // Battery idle screen-off (USB never turns off).
    if (!screenOff && !st.approvalActive && !snap.battery.usbPresent) {
      if (nowMs - lastInteractMs > SCREEN_OFF_BAT_MS) {
        hwDisplaySleep(true);
        screenOff = true;
      }
    }

    // Detect state changes requiring a full redraw.
    if (st.stateGen != lastStateGen || st.quotaGen != lastQuotaGen || st.rankGen != lastRankGen ||
        st.wifiConnected != lastWifiUp || st.wifiApMode != lastApMode ||
        st.mqttConnected != lastMqttUp || st.minute != lastMinute || st.battPct != lastBattPct) {
      lastStateGen = st.stateGen;
      lastQuotaGen = st.quotaGen;
      lastRankGen  = st.rankGen;
      lastWifiUp   = st.wifiConnected;
      lastApMode   = st.wifiApMode;
      lastMqttUp   = st.mqttConnected;
      lastMinute   = st.minute;
      lastBattPct  = st.battPct;
      needFull     = true;
    }
    if (st.approvalActive && st.second != lastSecond) {
      lastSecond = st.second;
      needFull = true;
    }

    // Drive Nova animation state from DSH status / one-shot click.
    if (clickAnimUntilMs != 0 && (int32_t)(nowMs - clickAnimUntilMs) < 0) {
      nvaSetState(NvaAnimKey::CLICK);
    } else {
      clickAnimUntilMs = 0;
      if (st.approvalActive || st.wait > 0) nvaSetState(NvaAnimKey::WAITING);
      else if (st.run > 0)                  nvaSetState(NvaAnimKey::WORKING);
      else                                  nvaSetState(NvaAnimKey::IDLE);
    }
    nvaTick(nowMs);

    // Frame rate target depends on active page & power profile.
    bool animPage = (!st.approvalActive && !st.wifiApMode && nav.currentPage() == UiPage::HOME);
    uint8_t targetFps = animPage ? settingsEffectiveFps(snap.battery.usbPresent) : 10;
    if (screenOff || dimmedForNap) targetFps = 5;
    int64_t frameUs = 1000000LL / targetFps;

    if (!screenOff && !dimmedForNap && canvas) {
      if (needFull) {
        int64_t t0 = esp_timer_get_time();
        nav.draw(*canvas, st, cfg, frame, nowMs);
        hwDisplayPush();
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needFull = false;
        frame++;
        fpsFrames++;
      } else if (animPage) {
        int16_t dx = 0, dy = 0, dw = 0, dh = 0;
        int64_t t0 = esp_timer_get_time();
        pageHomeDrawStage(*canvas, st, frame, &dx, &dy, &dw, &dh);
        hwDisplayPushRect(dx, dy, dw, dh);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        frame++;
        fpsFrames++;
      }
    }

    int64_t nowUs = esp_timer_get_time();
    int64_t elapsedUs = nowUs - fpsWindowStartUs;
    if (elapsedUs >= 1000000) {
      curFps = (float)fpsFrames * 1000000.0f / (float)elapsedUs;
      fpsFrames = 0;
      fpsWindowStartUs = nowUs;
    }

    nextFrameAt += frameUs;
    if (nextFrameAt < nowUs - frameUs) nextFrameAt = nowUs;
    int64_t waitUs = nextFrameAt - nowUs;
    if (waitUs > 1000) {
      vTaskDelay(pdMS_TO_TICKS((uint32_t)(waitUs / 1000)));
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

}  // namespace

void setup() {
  hwInit();
  settingsLoad();
  hwDisplayBrightness(settingsGet().brightLevel);

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char devId[16];
  snprintf(devId, sizeof(devId), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  novaStateInit(devId, FW_VER);
  Serial.printf("nova: deviceId=%s\n", devId);

  if (!nvaInit("/pets/nova")) {
    Serial.println("nvaInit: no .nva assets found, using fallback orb");
  }
  if (!hwBusStart()) {
    Serial.println("hwBusStart failed");
    while (1) delay(1000);
  }
  if (!netStart()) {
    Serial.println("netStart failed");
    while (1) delay(1000);
  }

  sfxPlay(SfxKind::NEW_EVENT, settingsGet().soundPct);
  xTaskCreatePinnedToCore(uiTaskFn, "task_ui", 10240, nullptr, 5, nullptr, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
