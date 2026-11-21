#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
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
#include "ui/theme.h"
#include "ui/transition.h"

namespace {

constexpr uint32_t KEY1_LONG_MS = 800;

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

void pollSerialCommands(UiNav& nav, bool& needFull,
                        uint32_t& fpsFrames, int64_t& fpsWindowStartUs, int64_t& nextFrameAt) {
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
            fpsFrames = 0;
            fpsWindowStartUs = esp_timer_get_time();
            nextFrameAt = fpsWindowStartUs;
          } else if (strcmp(cmd, "stat") == 0) {
            static NovaState snap;
            novaStateRead(&snap);
            Serial.printf("STAT fps=%.1f pushUs=%lu heap=%lu psram=%lu mhz=%u anim=%d\n",
                          snap.fps, (unsigned long)snap.pushUs,
                          (unsigned long)snap.freeHeap, (unsigned long)snap.freePsram,
                          (unsigned)snap.cpuMhz, (int)nvaActiveState());
          } else if (strcmp(cmd, "reprov") == 0) {
            netRequestReprovision();
          } else if (strcmp(cmd, "wifi") == 0) {
            const char* ssid = doc["ssid"] | "";
            const char* pass = doc["pass"] | "";
            if (ssid[0]) netRequestWifiConnect(ssid, pass);
          } else if (strcmp(cmd, "page") == 0) {
            const char* p = doc["page"] | "home";
            if (strcmp(p, "home") == 0)          { nav.setPage(UiPage::HOME); needFull = true; }
            else if (strcmp(p, "quota") == 0)    { nav.setPage(UiPage::QUOTA); needFull = true; }
            else if (strcmp(p, "rank") == 0)     { nav.setPage(UiPage::RANK); needFull = true; }
            else if (strcmp(p, "detail") == 0)   { nav.setPage(UiPage::DETAIL); needFull = true; }
            else if (strcmp(p, "settings") == 0) { pageSettingsSetTab(0); nav.setPage(UiPage::SETTINGS); needFull = true; }
            else if (strcmp(p, "settings_adv") == 0) { pageSettingsSetTab(1); nav.setPage(UiPage::SETTINGS); needFull = true; }
          } else if (strcmp(cmd, "demo") == 0) {
            int anim = doc["anim"] | -1;
            pageHomeSetDemoOverride(anim);
            needFull = true;
          }
        }
      } else if (strstr(line, "shot") != nullptr) {
        sendUsbScreenshot();
        fpsFrames = 0;
        fpsWindowStartUs = esp_timer_get_time();
        nextFrameAt = fpsWindowStartUs;
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
  esp_task_wdt_add(nullptr);
  Arduino_Canvas* canvas = hwCanvas();
  UiNav nav;
  nav.init(settingsGet().homePage);

  bool screenOff = false;
  bool dimmedForNap = false;
  bool needFull = true;
  bool uiAnimating = false;
  bool needRankList = false;
  bool wasRankFlinging = false;
  bool wasApprovalActive = false;

  int16_t tpStartX = 0, tpStartY = 0, tpLastY = 0;
  bool    tpActive = false, tpDraggedV = false;
  bool    hasPressedRect = false;
  ui::Rect pressedRect{ 0, 0, 0, 0 };
  uint32_t key1DownAt = 0;
  bool     key1LongFired = false;
  uint32_t lastInteractMs = millis();
  bool     clickAnimActive = false;
  uint32_t clickSafetyEndMs = 0;
  uint32_t lastTapVoiceMs = 0;
  bool     oneShotActive = false;
  uint32_t oneShotSafetyEndMs = 0;
  NvaAnimKey oneShotKey = NvaAnimKey::SUCCESS;
  NvaAnimKey bgAutoKey = NvaAnimKey::IDLE;
  NvaAnimKey bgLastNonIdle = NvaAnimKey::QUOTA;
  uint32_t   bgNextSwitchCycle = 2;

  uint32_t lastApGen = 0;
  uint32_t lastStateGen = 0;
  uint32_t lastQuotaGen = 0;
  uint32_t lastRankGen = 0;
  uint32_t lastChoreoGen = 0;
  uint16_t lastRunCount = 0;
  uint8_t  tapVoiceIdx = 0;
  bool     lastWifiUp = false;
  bool     lastApMode = false;
  bool     lastMqttUp = false;
  uint32_t lastMqttRxSeen = 0;
  bool     lastStaleFlag = false;
  uint32_t lastStaleShownSec = 0;
  uint32_t lastQuotaBucket = 0;
  uint8_t  lastMinute = 255;
  uint8_t  lastSecond = 255;
  uint32_t lastUrgentBreathMs = 0;
  uint8_t  lastBattPct = 255;
  uint8_t  lastBurnStep = 0;
  bool     lastSleepState = false;
  uint32_t curCpuMhz = getCpuFrequencyMhz();

  uint32_t frame = 0;
  uint32_t fpsFrames = 0;
  int64_t  fpsWindowStartUs = esp_timer_get_time();
  float    curFps = 0.0f;
  float    lastHomeFps = 60.0f;
  uint32_t lastPushUs = 0;
  int64_t  nextFrameAt = esp_timer_get_time();

  auto applyCpuMhz = [&](uint32_t targetMhz) {
    if (targetMhz != curCpuMhz) {
      if (setCpuFrequencyMhz(targetMhz)) curCpuMhz = targetMhz;
    }
  };

  for (;;) {
    esp_task_wdt_reset();
    uint32_t nowMs = millis();
    pollSerialCommands(nav, needFull, fpsFrames, fpsWindowStartUs, nextFrameAt);

    const NovaSettings& cfg = settingsGet();
    BusSnapshot snap = hwBusSnapshot();
    uint8_t burnStep = (uint8_t)((nowMs / 30000u) % 25u);

    // Update shared NovaState hardware fields.
    novaStateWithLock([&](NovaState& s) {
      s.fps        = (lastHomeFps > 0.5f) ? lastHomeFps : curFps;
      s.pushUs     = lastPushUs;
      s.freeHeap   = esp_get_free_heap_size();
      s.freePsram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      s.battMv     = snap.battery.mV;
      s.battPct    = snap.battery.pct;
      s.usbPresent = snap.battery.usbPresent;
      s.charging   = snap.battery.charging;
      s.faceDown   = snap.faceDown;
      s.hour       = snap.time.H;
      s.minute     = snap.time.M;
      s.second     = snap.time.S;
      s.timeValid  = snap.timeValid;
      s.nowMs      = nowMs;
      s.cpuMhz     = (uint16_t)curCpuMhz;
      s.burnInStep = burnStep;

      // Auto-expire approval on device side after TTL.
      if (s.approvalActive && s.apTtlMs > 0 && (nowMs - s.apArrivedMs >= s.apTtlMs)) {
        s.approvalActive = false;
        s.apGen++;
      }
    });

    static NovaState st;
    novaStateRead(&st);

    // CPU Dynamic Frequency Scaling (DFS via PLL 240/160/80 MHz, keeping APB >= 80 MHz).
    bool activeDemand = st.approvalActive || st.run > 0 || st.wait > 0 ||
                        ((nowMs - lastInteractMs) < 10000u);
    uint32_t wantMhz = 240;
    if (cfg.powerProfile == PowerProfile::LONG_BATT) {
      wantMhz = (screenOff || dimmedForNap) ? 80 : (st.approvalActive ? 160 : 80);
    } else if (cfg.powerProfile == PowerProfile::ADAPTIVE) {
      wantMhz = (screenOff || dimmedForNap || (!snap.battery.usbPresent && !activeDemand)) ? 80 : 240;
    }
    applyCpuMhz(wantMhz);

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

    auto playTransition = [&](TransKind kind, uint32_t ms) {
      uint16_t* from = nullptr;
      uint16_t* to   = nullptr;
      uint16_t* fb   = canvas ? canvas->getFramebuffer() : nullptr;
      if (screenOff || dimmedForNap || !canvas || !fb || !transBuffers(&from, &to)) {
        if (canvas && !screenOff && !dimmedForNap) {
          nav.draw(*canvas, st, cfg, frame, nowMs);
        }
        needFull = true;
        return;
      }
      const size_t fbBytes = (size_t)UI_W * UI_H * sizeof(uint16_t);
      memcpy(from, fb, fbBytes);
      applyCpuMhz(240);
      nav.draw(*canvas, st, cfg, frame, nowMs);
      memcpy(to, fb, fbBytes);
      uiAnimating = true;
      transRun(*canvas, from, to, kind, ms);
      uiAnimating = false;
      needFull = false;
      frame++;
      fpsFrames++;
    };

    // Handle approval arrival / dismissal transitions (placed after playTransition).
    if (st.apGen != lastApGen) {
      lastApGen = st.apGen;
      hasPressedRect = false;
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
        if (!wasApprovalActive) {
          wasApprovalActive = true;
          if (!screenOff && !dimmedForNap && !snap.faceDown) {
            playTransition(TransKind::FADE, 150);
          } else {
            needFull = true;
          }
        } else {
          needFull = true;
        }
      } else if (wasApprovalActive) {
        wasApprovalActive = false;
        playTransition(TransKind::FADE, 150);
      } else {
        needFull = true;
      }
    }

    // Raise WiFi power mode during approval or active interaction.
    bool needLowLatencyNet = st.approvalActive || st.wifiApMode || ((nowMs - lastInteractMs) < 5000);
    netSetPowerSave(needLowLatencyNet ? NetPsMode::PERFORMANCE : NetPsMode::LOW_POWER);

    auto handleAction = [&](const UiAction& act, int slideDir = 0, int swipeOrigin = -1) {
      if (act.kind == UiAction::NONE) return;
      if (act.kind != UiAction::PET_TAP || act.value == 2) needFull = true;
      switch (act.kind) {
        case UiAction::GO_PAGE: {
          UiPage prevPage = (swipeOrigin >= 0) ? (UiPage)swipeOrigin : nav.currentPage();
          UiPage newPage  = (UiPage)act.value;
          nav.setPage(newPage);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          if (prevPage != UiPage::SETTINGS && newPage == UiPage::SETTINGS) {
            playTransition(TransKind::SHEET_UP, 220);
          } else if (prevPage == UiPage::SETTINGS && newPage != UiPage::SETTINGS) {
            playTransition(TransKind::SHEET_DOWN, 220);
          } else if (slideDir != 0) {
            playTransition(slideDir > 0 ? TransKind::SLIDE_LEFT : TransKind::SLIDE_RIGHT, 180);
          }
          break;
        }
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
        case UiAction::SET_SOUND:
          settingsMut().soundPct = (uint8_t)((act.value < 0) ? 0 : (act.value > 100 ? 100 : act.value));
          settingsSave();
          netPublishPrefs();
          if (settingsGet().soundPct > 0) sfxPlay(SfxKind::NAV_CHIRP, settingsGet().soundPct);
          break;
        case UiAction::SET_BRIGHT:
          settingsMut().brightLevel = (uint8_t)act.value;
          settingsSave();
          hwDisplayBrightness(settingsGet().brightLevel);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_TTS:
          settingsMut().ttsEnabled = (act.value != 0);
          settingsSave();
          netPublishPrefs();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_QUIET:
          settingsMut().quietHours = (act.value != 0);
          settingsSave();
          netPublishPrefs();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_BATSLEEP:
          settingsMut().batSleepSec = (uint16_t)(act.value >= 10 ? act.value : 30);
          settingsSave();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_CHOREO:
          settingsMut().choreoEnabled = (act.value != 0);
          settingsSave();
          netPublishPrefs();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::TEST_SOUND:
          if (!sfxSpeakFile("/audio/tap0.adp", cfg.soundPct, st.hour, st.timeValid, cfg.quietHours)) {
            sfxPlay(SfxKind::NEW_EVENT, cfg.soundPct);
          }
          break;
        case UiAction::SYNC_HOST:
          netPublishSync();
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
          if (act.value == 0) {
            static constexpr const char* kTapVoices[3] = {
              "/audio/tap0.adp", "/audio/tap1.adp", "/audio/tap2.adp"
            };
            clickAnimActive = true;
            clickSafetyEndMs = nowMs + 13500u;
            nvaSetState(NvaAnimKey::CLICK, true, true);
            if ((nowMs - lastTapVoiceMs) >= 800u) {
              lastTapVoiceMs = nowMs;
              if (!sfxSpeakFile(kTapVoices[tapVoiceIdx % 3], cfg.soundPct, st.hour, st.timeValid, cfg.quietHours)) {
                sfxPlay(SfxKind::NEW_EVENT, cfg.soundPct);
              }
              tapVoiceIdx++;
            }
          } else if (act.value == 1) {
            if ((nowMs - lastTapVoiceMs) >= 800u) {
              lastTapVoiceMs = nowMs;
              if (!sfxSpeakFile("/audio/run.adp", cfg.soundPct, st.hour, st.timeValid, cfg.quietHours)) {
                sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
              }
            }
          } else {
            sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          }
          break;
        case UiAction::APPROVE:
          netPublishDecision(st.apId, true, false);
          novaStateWithLock([](NovaState& s) { s.approvalActive = false; s.apGen++; });
          novaStateRead(&st);
          lastApGen = st.apGen;
          wasApprovalActive = false;
          oneShotKey = NvaAnimKey::SUCCESS;
          oneShotActive = true;
          oneShotSafetyEndMs = nowMs + 13500u;
          nvaSetState(oneShotKey, true, true);
          sfxPlay(SfxKind::ACCEPT, cfg.soundPct);
          playTransition(TransKind::FADE, 150);
          break;
        case UiAction::DENY:
          netPublishDecision(st.apId, false, false);
          novaStateWithLock([](NovaState& s) { s.approvalActive = false; s.apGen++; });
          novaStateRead(&st);
          lastApGen = st.apGen;
          wasApprovalActive = false;
          oneShotKey = NvaAnimKey::ERROR;
          oneShotActive = true;
          oneShotSafetyEndMs = nowMs + 8500u;
          nvaSetState(oneShotKey, true, true);
          sfxPlay(SfxKind::DENY, cfg.soundPct);
          playTransition(TransKind::FADE, 150);
          break;
        case UiAction::TO_PC:
          netPublishDecision(st.apId, false, true);
          novaStateWithLock([](NovaState& s) { s.approvalActive = false; s.apGen++; });
          novaStateRead(&st);
          lastApGen = st.apGen;
          wasApprovalActive = false;
          oneShotKey = NvaAnimKey::THINKING;
          oneShotActive = true;
          oneShotSafetyEndMs = nowMs + 13500u;
          nvaSetState(oneShotKey, true, true);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          playTransition(TransKind::FADE, 150);
          break;
        case UiAction::RANK_REDRAW:
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          playTransition(TransKind::FADE, 150);
          break;
        case UiAction::SETTINGS_REDRAW:
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          if (act.value == 0 || act.value == 1) {
            playTransition(TransKind::FADE, 150);
          }
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
        if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
          pageRankTouchDown(ev.y, ev.tsMs);
        }
        if (!dimmedForNap && canvas && nav.pressedRect(ev.x, ev.y, st, cfg, &pressedRect)) {
          ui::drawPressedOverlay(*canvas, pressedRect, 12);
          hwDisplayPushRect(pressedRect.x, pressedRect.y, pressedRect.w, pressedRect.h);
          hasPressedRect = true;
        } else {
          hasPressedRect = false;
        }
      } else if (ev.type == InputEvtType::TOUCH_MOVE && tpActive) {
        wakeScreen();
        int16_t adx = ev.x > tpStartX ? (ev.x - tpStartX) : (tpStartX - ev.x);
        int16_t ady = ev.y > tpStartY ? (ev.y - tpStartY) : (tpStartY - ev.y);
        if (hasPressedRect && (adx >= 12 || ady >= 12)) {
          hasPressedRect = false;
          needFull = true;
        }
        if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
          if (!tpDraggedV && ady >= 12 && ady > adx) {
            tpDraggedV = true;
          }
          if (tpDraggedV) {
            if (pageRankTouchMove(ev.y, ev.tsMs, st.nRows)) {
              needRankList = true;
            }
          }
        }
        tpLastY = ev.y;
      } else if (ev.type == InputEvtType::TOUCH_UP && tpActive) {
        tpActive = false;
        wakeScreen();
        if (hasPressedRect) {
          hasPressedRect = false;
          needFull = true;
        }
        if (tpDraggedV) {
          tpDraggedV = false;
          if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
            pageRankTouchUp(ev.tsMs, st.nRows);
            if (!pageRankIsFlinging()) {
              needFull = true;
            }
          }
          continue;
        }
        if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
          pageRankTouchUp(ev.tsMs, st.nRows);
        }
        int16_t dx = ev.x - tpStartX;
        int16_t dy = ev.y - tpStartY;
        UiPage prevPage = nav.currentPage();
        UiAction act = st.approvalActive ? UiAction{} : nav.onSwipe(dx, dy);
        if (act.kind != UiAction::NONE) {
          int16_t adx = dx < 0 ? -dx : dx;
          int slideDir = (prevPage == UiPage::SETTINGS && dy >= 60 && dy > adx)
                           ? 0
                           : (dx < 0 ? 1 : -1);
          handleAction(act, slideDir, (int)prevPage);
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
          UiPage prev = nav.currentPage();
          nav.goHome(cfg.homePage);
          UiPage next = nav.currentPage();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          if (next != prev) {
            TransKind kind = ((int)next > (int)prev)
                               ? TransKind::SLIDE_LEFT
                               : TransKind::SLIDE_RIGHT;
            playTransition(kind, 180);
          }
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
      if (nav.currentPage() != UiPage::SETTINGS) {
        nav.setPage(UiPage::SETTINGS);
        sfxPlay(SfxKind::NEW_EVENT, cfg.soundPct);
        playTransition(TransKind::SHEET_UP, 220);
      } else {
        sfxPlay(SfxKind::NEW_EVENT, cfg.soundPct);
      }
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
    const uint32_t batOffMs = (uint32_t)(cfg.batSleepSec >= 10 ? cfg.batSleepSec : 30) * 1000u;
    if (!screenOff && !st.approvalActive && !snap.battery.usbPresent) {
      if (nowMs - lastInteractMs > batOffMs) {
        hwDisplaySleep(true);
        screenOff = true;
      }
    }

    // Detect task completion to celebrate cleanly or show error expression.
    if (lastRunCount > 0 && st.run == 0 && !st.approvalActive) {
      if (st.turnError) {
        oneShotKey = NvaAnimKey::ERROR;
        oneShotSafetyEndMs = nowMs + 8500u;
      } else {
        oneShotKey = NvaAnimKey::SUCCESS;
        oneShotSafetyEndMs = nowMs + 13500u;
      }
      oneShotActive = true;
      nvaSetState(oneShotKey, true, true);
    }
    lastRunCount = st.run;

    bool isStale = (st.wifiConnected && st.mqttConnected && st.lastMqttRxMs > 0 &&
                    nowMs > st.lastMqttRxMs && (nowMs - st.lastMqttRxMs) > 45000);
    uint32_t staleBucket = isStale ? ((nowMs - st.lastMqttRxMs) / 5000) : 0;
    uint32_t quotaBucket = (nav.currentPage() == UiPage::QUOTA && st.quotaRxMs > 0 && nowMs > st.quotaRxMs)
                             ? ((nowMs - st.quotaRxMs) / 15000u)
                             : 0u;
    bool curSleepState = (nvaActiveState() == NvaAnimKey::SLEEP);

    // Detect state / heartbeat / burn-in pixel shift changes requiring a full redraw.
    if (st.stateGen != lastStateGen || st.quotaGen != lastQuotaGen || st.rankGen != lastRankGen ||
        st.choreoGen != lastChoreoGen ||
        st.wifiConnected != lastWifiUp || st.wifiApMode != lastApMode ||
        st.mqttConnected != lastMqttUp || st.minute != lastMinute || st.battPct != lastBattPct ||
        st.lastMqttRxMs != lastMqttRxSeen || isStale != lastStaleFlag ||
        staleBucket != lastStaleShownSec || quotaBucket != lastQuotaBucket ||
        burnStep != lastBurnStep || curSleepState != lastSleepState) {
      lastStateGen      = st.stateGen;
      lastQuotaGen      = st.quotaGen;
      lastRankGen       = st.rankGen;
      lastChoreoGen     = st.choreoGen;
      lastWifiUp        = st.wifiConnected;
      lastApMode        = st.wifiApMode;
      lastMqttUp        = st.mqttConnected;
      lastMqttRxSeen    = st.lastMqttRxMs;
      lastStaleFlag     = isStale;
      lastStaleShownSec = staleBucket;
      lastQuotaBucket   = quotaBucket;
      lastMinute        = st.minute;
      lastBattPct       = st.battPct;
      lastBurnStep      = burnStep;
      lastSleepState    = curSleepState;
      needFull          = true;
    }
    if (st.approvalActive && !screenOff && !dimmedForNap) {
      if (st.second != lastSecond) {
        lastSecond = st.second;
        needFull = true;
      }
      uint32_t apElapsed = (nowMs >= st.apArrivedMs) ? (nowMs - st.apArrivedMs) : 0;
      uint32_t apTtl = st.apTtlMs ? st.apTtlMs : 15000u;
      uint32_t apRem = (apElapsed >= apTtl) ? 0 : (apTtl - apElapsed);
      // Sample 4 phases of the 2Hz breathing border cleanly without AA border accumulation.
      if (apRem <= 3000u && (nowMs - lastUrgentBreathMs) >= 125u) {
        lastUrgentBreathMs = nowMs;
        needFull = true;
      }
    }
    if (pageSettingsTick(nowMs) && nav.currentPage() == UiPage::SETTINGS) {
      needFull = true;
    }

    // Drive all 9 Nova animation states cleanly: play one-shots & choreo steps to full cycle end
    // (inspired by dsh-pet AnimationFlow & aipets Director / ClipSelector).
    int demoIdx = pageHomeDemoOverride();
    int choreoAnim = pageHomeChoreoAnim();
    if (st.approvalActive || st.wait > 0) {
      clickAnimActive = false;
      oneShotActive = false;
      nvaSetState(NvaAnimKey::WAITING, false);
    } else if (demoIdx >= 0 && demoIdx <= 8) {
      clickAnimActive = false;
      oneShotActive = false;
      nvaSetState((NvaAnimKey)demoIdx, false);
    } else if (clickAnimActive) {
      if ((nvaActiveState() == NvaAnimKey::CLICK && nvaIsEnded()) ||
          (int32_t)(nowMs - clickSafetyEndMs) >= 0) {
        clickAnimActive = false;
      } else {
        nvaSetState(NvaAnimKey::CLICK, true);
      }
    } else if (oneShotActive) {
      if ((nvaActiveState() == oneShotKey && nvaIsEnded()) ||
          (int32_t)(nowMs - oneShotSafetyEndMs) >= 0) {
        oneShotActive = false;
      } else {
        nvaSetState(oneShotKey, true);
      }
    }

    if (!st.approvalActive && st.wait == 0 && demoIdx < 0 && !clickAnimActive && !oneShotActive) {
      if (st.run > 0) {
        nvaSetState(NvaAnimKey::WORKING, false);
      } else if (pageHomeIsWalking()) {
        nvaSetState(NvaAnimKey::THINKING, false);
      } else if ((nowMs - lastInteractMs) >= (snap.battery.usbPresent ? 90000u : 45000u)) {
        nvaSetState(NvaAnimKey::SLEEP, false);
      } else if (choreoAnim >= 0 && choreoAnim <= 8) {
        nvaSetState((NvaAnimKey)choreoAnim, false);
      } else {
        uint32_t cyc = nvaLoopCycles();
        if (cyc >= bgNextSwitchCycle) {
          if (bgAutoKey == NvaAnimKey::IDLE) {
            NvaAnimKey next = (st.sess > 0 && bgLastNonIdle != NvaAnimKey::THINKING)
                                ? NvaAnimKey::THINKING
                                : (bgLastNonIdle != NvaAnimKey::QUOTA ? NvaAnimKey::QUOTA : NvaAnimKey::WAITING);
            bgAutoKey = next;
            bgLastNonIdle = next;
            bgNextSwitchCycle = cyc + 1;  // play 1 full ~10s cycle before switching back
          } else {
            bgAutoKey = NvaAnimKey::IDLE;
            bgNextSwitchCycle = cyc + 2;  // rest in IDLE for 2 full cycles (~20s)
          }
        }
        nvaSetState(bgAutoKey, false);
      }
    }
    nvaTick(nowMs);

    if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
      if (pageRankTick(nowMs)) {
        needRankList = true;
      }
      bool curFlinging = pageRankIsFlinging();
      if (wasRankFlinging && !curFlinging) {
        needFull = true;
      }
      wasRankFlinging = curFlinging;
    } else {
      if (pageRankIsFlinging()) pageRankTick(nowMs);
      wasRankFlinging = false;
    }

    // Frame rate target depends on active page & power profile.
    bool animPage = (!st.approvalActive && !st.wifiApMode && nav.currentPage() == UiPage::HOME);
    bool interactive = tpActive || pageRankIsFlinging() || uiAnimating;
    uint8_t targetFps = animPage
                          ? settingsEffectiveFps(snap.battery.usbPresent)
                          : (interactive ? 60 : (st.approvalActive ? 30 : 10));
    if (screenOff || dimmedForNap) targetFps = 5;
    int64_t frameUs = 1000000LL / targetFps;

    if (!screenOff && !dimmedForNap && canvas) {
      if (needFull) {
        int64_t t0 = esp_timer_get_time();
        nav.draw(*canvas, st, cfg, frame, nowMs);
        hwDisplayPush();
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needFull = false;
        needRankList = false;
        frame++;
        fpsFrames++;
      } else if (needRankList && !st.approvalActive && nav.currentPage() == UiPage::RANK) {
        int64_t t0 = esp_timer_get_time();
        pageRankDrawList(*canvas, st);
        hwDisplayPushRect(0, 96, 368, 295);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needRankList = false;
        frame++;
        fpsFrames++;
      } else if (animPage) {
        int16_t dx = 0, dy = 0, dw = 0, dh = 0;
        int64_t t0 = esp_timer_get_time();
        pageHomeDrawStage(*canvas, st, cfg, frame, &dx, &dy, &dw, &dh);
        hwDisplayPushRect(dx, dy, dw, dh);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        frame++;
        fpsFrames++;
      } else if (st.approvalActive) {
        int64_t t0 = esp_timer_get_time();
        pageApprovalDrawBar(*canvas, st, nowMs);
        hwDisplayPushRect(36, 92, 296, 8);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        frame++;
        fpsFrames++;
      }
    }

    int64_t nowUs = esp_timer_get_time();
    int64_t elapsedUs = nowUs - fpsWindowStartUs;
    if (elapsedUs >= 1000000) {
      curFps = (float)fpsFrames * 1000000.0f / (float)elapsedUs;
      if (animPage && !screenOff && !dimmedForNap && curFps > 5.0f) {
        lastHomeFps = curFps;
      }
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
  novaStateInit(devId, NOVA_FW_VER);
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

  BusSnapshot initSnap = hwBusSnapshot();
  if (!sfxSpeakFile("/audio/boot.adp", settingsGet().soundPct, initSnap.time.H,
                    initSnap.timeValid, settingsGet().quietHours)) {
    sfxPlay(SfxKind::NEW_EVENT, settingsGet().soundPct);
  }
  xTaskCreatePinnedToCore(uiTaskFn, "task_ui", 10240, nullptr, 5, nullptr, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
