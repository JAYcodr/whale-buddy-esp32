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

// 串口 tap 注入：串口任务写、UI 任务读。坐标是独立标量，
// 即使读到中间的写也不会画错，最多点歪一格，所以 volatile 足够。
volatile int8_t  s_aodForceReq = -1;   // 串口 {"cmd":"aodforce","on":0|1}，V0.5.0 验收用
volatile bool    s_pendingTap  = false;
volatile int16_t s_pendingTapX = 0;
volatile int16_t s_pendingTapY = 0;

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
            Serial.printf("STAT fps=%.1f drawFps=%.1f pushUs=%lu idle=%lu draws=%lu "
                          "heap=%lu psram=%lu mhz=%u anim=%d pwr=%u brt=%u lcd=%u aod=%u "
                          "cpu=%u/%u bat=%u%% %umV stage=%u chg=%umA tmp=%dC usb=%d\n",
                          snap.fps, snap.drawFps, (unsigned long)snap.pushUs,
                          (unsigned long)(snap.nowMs > snap.lastDrawMs ? snap.nowMs - snap.lastDrawMs : 0),
                          (unsigned long)snap.drawCount,
                          (unsigned long)snap.freeHeap, (unsigned long)snap.freePsram,
                          (unsigned)snap.cpuMhz, (int)nvaActiveState(),
                          (unsigned)snap.pwrProfile, (unsigned)snap.effBright,
                          snap.lcdOff ? 0u : 1u, snap.aodActive ? 1u : 0u,
                          (unsigned)snap.uiBusyPct, (unsigned)snap.decBusyPct,
                          (unsigned)snap.battPct, (unsigned)snap.battMv,
                          (unsigned)snap.battStage, (unsigned)snap.chgMa,
                          (int)snap.battTempC, snap.usbPresent ? 1 : 0);
          } else if (strcmp(cmd, "bat") == 0) {
            // Machine-readable power dump for the real-hardware acceptance run.
            HwBattery b = hwBattery();
            Serial.printf("{\"bat\":{\"pct\":%d,\"mv\":%d,\"stage\":%u,\"chg\":%u,"
                          "\"limited\":%s,\"usb\":%s,\"tmp\":%d,\"gauge\":%d}}\n",
                          b.pct, b.mV, (unsigned)b.stage, (unsigned)b.chgMa,
                          b.chargeLimited ? "true" : "false",
                          b.usbPresent ? "true" : "false",
                          b.tempC, (int)b.gaugePct);
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
            else if (strcmp(p, "settings_power") == 0) { pageSettingsSetTab(2); nav.setPage(UiPage::SETTINGS); needFull = true; }
            else if (strcmp(p, "settings_display") == 0) { pageSettingsSetTab(3); nav.setPage(UiPage::SETTINGS); needFull = true; }
            else if (strcmp(p, "tokens") == 0)   { pageTokensOnEnter(millis()); nav.setPage(UiPage::TOKENS); needFull = true; }
          } else if (strcmp(cmd, "tap") == 0) {
            // 真机验收用：把手动戳屏变成可重放的命令。
            // UI 任务里才拿得到 nav / handleAction，这里只放下坐标。
            s_pendingTapX = (int16_t)(doc["x"] | 0);
            s_pendingTapY = (int16_t)(doc["y"] | 0);
            s_pendingTap = true;
          } else if (strcmp(cmd, "demo") == 0) {
            int anim = doc["anim"] | -1;
            pageHomeSetDemoOverride(anim);
            needFull = true;
          } else if (strcmp(cmd, "statrate") == 0) {
            // 采数用：临时把 stat 上行从 30s 改密（5..300s，0=默认）
            netSetStatIntervalMs((uint16_t)(doc["ms"] | 0));
          } else if (strcmp(cmd, "pwr") == 0) {
            int pr = doc["profile"] | 0;
            if (pr < 0 || pr > 2) pr = 0;
            settingsMut().powerProfile = (PowerProfile)pr;
            settingsSave();
            needFull = true;
          } else if (strcmp(cmd, "aod") == 0) {
            int am = doc["on"] | 255;
            if (am >= 0 && am <= 2) {
              settingsMut().aodMode = (uint8_t)am;
              settingsSave();
              needFull = true;
            }
          } else if (strcmp(cmd, "aodforce") == 0) {
            s_aodForceReq = (doc["on"] | 0) ? 1 : 0;
          } else if (strcmp(cmd, "aodidle") == 0) {
            // 验收用：把进入阈值临时改短（策略层下限 30s）
            int sec = doc["sec"] | 120;
            if (sec < 30) sec = 30;
            if (sec > 600) sec = 600;
            settingsMut().aodIdleSec = (uint16_t)sec;
            settingsSave();
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
  bool wasTokensAnim = false;
  bool needTokensSettle = false;

  // v0.5.0：AOD + 观测
  bool     aodActive = false;
  bool     aodForced = false;       // 串口 aodforce 打开的验收模式
  uint32_t aodEnterMs = 0;
  bool     swallowTap = false;      // 唤醒 AOD 的那一击不派发
  uint8_t  lastAppliedBright = 255;
  uint32_t uiBusyUs = 0;
  uint32_t decBusyUs = 0;
  uint32_t lastDecBusyUs = 0;
  uint8_t  uiBusyPct = 0;
  uint8_t  decBusyPct = 0;
  uint32_t drawCount = 0;
  uint32_t lastDrawMs = 0;
  float    drawFps = 0.0f;

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
  uint32_t lastTokenGen = 0;
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
  uint32_t lastBattDrawMs = 0;
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

  // 亮度去重：只有有效档变化才写 I2C（AOD 降档 + 熄屏前预降会频繁调用）。
  auto applyBright = [&](uint8_t lvl) {
    if (lvl > 4) lvl = 4;
    if (lvl == lastAppliedBright) return;
    lastAppliedBright = lvl;
    hwDisplayBrightness(lvl);
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
      // curFps 才是当前页的真实帧率；lastHomeFps 是主屏残留，
      // 在别的页读 stat 会被它骗（曾把 40ms/帧 报成 60fps）。
      s.fps        = curFps;
      s.pushUs     = lastPushUs;
      s.freeHeap   = esp_get_free_heap_size();
      s.freePsram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      s.battMv     = snap.battery.mV;
      s.battPct    = snap.battery.pct;
      s.usbPresent = snap.battery.usbPresent;
      s.charging   = snap.battery.charging;
      s.battStage      = snap.battery.stage;
      s.battTempC      = (int8_t)snap.battery.tempC;
      s.chgMa          = snap.battery.chgMa;
      s.chargeLimited  = snap.battery.chargeLimited;
      s.gaugePct       = snap.battery.gaugePct;
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

    // ── AOD（熄屏待机）状态机 ────────────────────────────────────────
    // 进入阈值：电池常开时压到 batSleepSec，否则「30 秒先熄屏、120 秒才想进 AOD」永远轮不到。
    uint32_t aodThresholdMs = aod::idleMsFor(cfg.aodIdleSec);
    if (cfg.aodMode == aod::ALWAYS && !snap.battery.usbPresent) {
      const uint32_t sleepMs =
          (uint32_t)(cfg.batSleepSec >= 10 ? cfg.batSleepSec : 30) * 1000u;
      if (sleepMs < aodThresholdMs) aodThresholdMs = sleepMs;
    }
    if (s_aodForceReq >= 0) {
      aodForced = (s_aodForceReq == 1);
      s_aodForceReq = -1;
      if (aodForced && !aodActive) {
        aodActive = true;
        aodEnterMs = nowMs;
        lastAppliedBright = 255;
        needFull = true;
        nvaSetState(NvaAnimKey::SLEEP, false);
      } else if (!aodForced && aodActive) {
        aodActive = false;
        lastAppliedBright = 255;
        needFull = true;
      }
    }
    if (aodActive) {
      const bool exitNow = !aodForced &&
          (aod::shouldExit(cfg.aodMode, snap.battery.usbPresent, st.run, st.wait,
                           st.approvalActive, st.wifiApMode, screenOff, st.battPct) ||
           nav.currentPage() != UiPage::HOME);
      if (exitNow) {
        aodActive = false;
        lastAppliedBright = 255;
        needFull = true;
      }
    } else if (!aodForced && aod::shouldEnter(cfg.aodMode, snap.battery.usbPresent, st.run, st.wait,
                                st.approvalActive, st.wifiApMode, screenOff, dimmedForNap,
                                nowMs - lastInteractMs, aodThresholdMs)) {
      aodActive = true;
      aodEnterMs = nowMs;
      lastAppliedBright = 255;
      needFull = true;
      nvaSetState(NvaAnimKey::SLEEP, false);
    }

    // 观测字段：探针从 stat 读这些，别让它们跟主状态错过一整轮。
    novaStateWithLock([&](NovaState& s) {
      s.month = snap.time.Mo;
      s.day = snap.time.D;
      s.dow = snap.time.dow;
      s.aodActive = aodActive;
      s.pwrProfile = (uint8_t)cfg.powerProfile;
      s.effBright = lastAppliedBright;
      s.lcdOff = (screenOff || dimmedForNap);
      s.uiBusyPct = uiBusyPct;
      s.decBusyPct = decBusyPct;
      s.drawFps = drawFps;
      s.lastDrawMs = lastDrawMs;
      s.drawCount = drawCount;
    });

    // CPU Dynamic Frequency Scaling (DFS via PLL 240/160/80 MHz, keeping APB >= 80 MHz).
    bool activeDemand = st.approvalActive || st.run > 0 || st.wait > 0 ||
                        ((nowMs - lastInteractMs) < 10000u);
    uint32_t wantMhz = 240;
    if (aodActive) {
      wantMhz = 80;   // 静止画面 + 4fps 轮询，80MHz 足够
    } else if (cfg.powerProfile == PowerProfile::LONG_BATT) {
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
        screenOff = false;
        lastAppliedBright = 255;   // 面板重开后必须重刷一次
        needFull = true;
      }
      if (dimmedForNap) {
        dimmedForNap = false;
        lastAppliedBright = 255;
        needFull = true;
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
          screenOff = false;
          lastAppliedBright = 255;
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
          if (newPage == UiPage::TOKENS && prevPage != UiPage::TOKENS) {
            pageTokensOnEnter(nowMs);
          }
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
          lastAppliedBright = 255;   // 交给统一亮度逻辑落地
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
        case UiAction::SET_CHGLIMIT:
          settingsMut().chargeLimitPct =
              (uint8_t)((act.value == 80 || act.value == 90 || act.value == 100) ? act.value : 90);
          settingsSave();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_AOD_MODE:
          settingsMut().aodMode =
              (uint8_t)((act.value >= 0 && act.value <= 2) ? act.value : (int)aod::USB);
          settingsSave();
          lastAppliedBright = 255;
          needFull = true;
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_AOD_IDLE:
          settingsMut().aodIdleSec =
              (uint16_t)((act.value == 60 || act.value == 120 || act.value == 300) ? act.value : 120);
          settingsSave();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_AOD_NIGHT:
          settingsMut().aodNightCap =
              (uint8_t)((act.value >= 0 && act.value <= 2) ? act.value : 1);
          settingsSave();
          lastAppliedBright = 255;
          needFull = true;
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::SET_CHGSPEED:
          settingsMut().chargeSpeed = (ChargeSpeed)((act.value >= 0 && act.value <= 2) ? act.value : 1);
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
        case UiAction::TOKENS_REDRAW:
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::DOCK_TOGGLE:
          novaStateWithLock([](NovaState& s) { s.dockExpanded = !s.dockExpanded; });
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          needFull = true;
          break;
        default: break;
      }
    };

    // 串口注入的点击：复用 handleAction，与真实触屏同一条路径。
    if (s_pendingTap) {
      const int16_t tx = s_pendingTapX;
      const int16_t ty = s_pendingTapY;
      s_pendingTap = false;
      if (aodActive) {
        aodActive = false;
        aodForced = false;
        lastAppliedBright = 255;
        needFull = true;
      } else {
        UiAction act = nav.onTap(tx, ty, st, cfg);
        if (act.kind != UiAction::NONE) {
          handleAction(act);
        } else {
          needFull = true;  // 至少重绘一次，方便截图对比
        }
      }
    }

    InputEvent ev;
    while (hwBusPollInput(&ev)) {
      if (ev.type == InputEvtType::TOUCH_DOWN) {
        if (screenOff) {
          wakeScreen();
          tpActive = false;
          continue;
        }
        if (aodActive) {
          aodActive = false;
          aodForced = false;
          lastAppliedBright = 255;
          needFull = true;
          swallowTap = true;
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
        if (!swallowTap && !dimmedForNap && canvas &&
            nav.pressedRect(ev.x, ev.y, st, cfg, &pressedRect)) {
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
        if (swallowTap) {
          swallowTap = false;
          continue;
        }
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
        if (aodActive) {
          aodActive = false;
          aodForced = false;
          lastAppliedBright = 255;
          needFull = true;
        }
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
            TransKind kind = (uiPageRingIndex(next) > uiPageRingIndex(prev))
                               ? TransKind::SLIDE_LEFT
                               : TransKind::SLIDE_RIGHT;
            playTransition(kind, 180);
          }
        }
        key1DownAt = 0;
      } else if (ev.type == InputEvtType::AXP_SHORT) {
        if (aodActive) {
          aodActive = false;
          aodForced = false;
          lastAppliedBright = 255;
          needFull = true;
        } else if (screenOff) {
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

    // Face-down nap dim/wake (skip during approval)。亮度统一在下面 applyBright。
    if (!st.approvalActive && !screenOff) {
      if (snap.faceDown && !dimmedForNap) {
        dimmedForNap = true;
        lastAppliedBright = 255;
      } else if (!snap.faceDown && dimmedForNap) {
        dimmedForNap = false;
        lastAppliedBright = 255;
        needFull = true;
      }
    }

    // 统一亮度：暗屏 0 / AOD 降档 / 平时用户档；只在有效值变化时写 I2C。
    {
      uint8_t wantBright = cfg.brightLevel;
      if (dimmedForNap) {
        wantBright = 0;
      } else if (aodActive) {
        wantBright = aod::brightness(cfg.brightLevel, cfg.aodNightCap, aod::isNight(st.hour));
      }
      applyBright(wantBright);
    }

    // Battery idle screen-off (USB never turns off)。
    // AOD 常开时由 AOD 接管主屏这块屏，但只在主屏、且电量还够的时候接管：
    // 低电自救 / 别的页 / 审批时仍然按 batSleepSec 熄屏，别把电池耗干。
    const bool aodCoversBattery = (cfg.aodMode == aod::ALWAYS) &&
                                  nav.currentPage() == UiPage::HOME &&
                                  st.battPct > aod::kLowBattPct;
    const uint32_t batOffMs = (uint32_t)(cfg.batSleepSec >= 10 ? cfg.batSleepSec : 30) * 1000u;
    if (!screenOff && !st.approvalActive && !snap.battery.usbPresent && !aodCoversBattery) {
      if (nowMs - lastInteractMs > batOffMs) {
        hwDisplaySleep(true);
        screenOff = true;
      }
    }

    // The charge governor lowers current while the panel is lit.
    hwPowerSetScreenOn(!(screenOff || dimmedForNap));

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

    // Battery percentage is smoothed by the estimator, but a 1% change still
    // costs a full-screen push. Redraw immediately on a 5% bucket change,
    // otherwise at most every 15 s.
    bool battChanged = (st.battPct != lastBattPct) &&
                       ((st.battPct / 5u) != (lastBattPct / 5u) ||
                        (nowMs - lastBattDrawMs) >= 15000u);

    // Detect state / heartbeat / burn-in pixel shift changes requiring a full redraw.
    if (st.stateGen != lastStateGen || st.quotaGen != lastQuotaGen || st.rankGen != lastRankGen ||
        st.choreoGen != lastChoreoGen || st.tokenGen != lastTokenGen ||
        st.wifiConnected != lastWifiUp || st.wifiApMode != lastApMode ||
        st.mqttConnected != lastMqttUp || st.minute != lastMinute || battChanged ||
        (!aodActive && st.lastMqttRxMs != lastMqttRxSeen) || isStale != lastStaleFlag ||
        staleBucket != lastStaleShownSec || quotaBucket != lastQuotaBucket ||
        burnStep != lastBurnStep || curSleepState != lastSleepState) {
      lastStateGen      = st.stateGen;
      lastQuotaGen      = st.quotaGen;
      lastRankGen       = st.rankGen;
      lastChoreoGen     = st.choreoGen;
      lastTokenGen      = st.tokenGen;
      lastWifiUp        = st.wifiConnected;
      lastApMode        = st.wifiApMode;
      lastMqttUp        = st.mqttConnected;
      lastMqttRxSeen    = st.lastMqttRxMs;
      lastStaleFlag     = isStale;
      lastStaleShownSec = staleBucket;
      lastQuotaBucket   = quotaBucket;
      lastMinute        = st.minute;
      lastBattPct       = st.battPct;
      lastBattDrawMs    = nowMs;
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
    if (st.dockExpanded && st.turnError && st.errMsg[0] && !screenOff) {
      needFull = true;  // 浮层期间锁全量重绘，压住宠物动画
    }
    if (pageSettingsTick(nowMs) && nav.currentPage() == UiPage::SETTINGS) {
      needFull = true;
    }
    if (pageQuotaTick(nowMs) && nav.currentPage() == UiPage::QUOTA) {
      needFull = true;
    }

    // Drive all 9 Nova animation states cleanly: play one-shots & choreo steps to full cycle end
    // (inspired by dsh-pet AnimationFlow & aipets Director / ClipSelector).
    int demoIdx = pageHomeDemoOverride();
    int choreoAnim = pageHomeChoreoAnim();
    if (!aodActive && (st.approvalActive || st.wait > 0)) {
      clickAnimActive = false;
      oneShotActive = false;
      nvaSetState(NvaAnimKey::WAITING, false);
    } else if (aodActive) {
      // AOD 期间宠物定格，任何自动状态机都不许改它。
      nvaSetState(NvaAnimKey::SLEEP, false);
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

    if (!aodActive && !st.approvalActive && st.wait == 0 && demoIdx < 0 &&
        !clickAnimActive && !oneShotActive) {
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
    // AOD 期间不跑动画：进入后先让 SLEEP 帧解码 0.9s，然后冻结在那一帧。
    if (!aodActive || (uint32_t)(nowMs - aodEnterMs) < 900u) {
      nvaTick(nowMs);
    }
    {
      const uint32_t decTotal = nvaDecodeBusyUs();
      decBusyUs += decTotal - lastDecBusyUs;
      lastDecBusyUs = decTotal;
    }

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
    // 异常详情浮层开着时不能走舞台局部重绘：pageHomeDrawStage 只重画宠物那一块，
    // 浮层画完立刻会被动画覆盖（真机实测：宠物和气泡压在浮层上）。
    bool animPage = (!aodActive && !st.approvalActive && !st.wifiApMode &&
                     nav.currentPage() == UiPage::HOME &&
                     !(st.dockExpanded && st.turnError && st.errMsg[0]));
    bool tokensPage = (!st.approvalActive && !st.wifiApMode && nav.currentPage() == UiPage::TOKENS);
    bool tokensAnim = tokensPage && pageTokensIsAnimating(st, cfg, nowMs);
    if (wasTokensAnim && !tokensAnim && tokensPage) {
      needTokensSettle = true;
    }
    wasTokensAnim = tokensAnim;

    bool interactive = tpActive || pageRankIsFlinging() || uiAnimating;
    // TOKENS 页一帧要搬 63 KB（368×88）@ 实测 2.7 MB/s ≈ 23 ms，喂不到 60；
    // 鱼又是 ±2.6px / 4s 的慢速微动，30 fps 和 60 fps 肉眼没差别，锁 30 换稳定。
    const uint8_t baseFps = settingsEffectiveFps(snap.battery.usbPresent);
    const uint8_t tokensFps = baseFps > 30 ? 30 : baseFps;
    uint8_t targetFps = animPage
                          ? baseFps
                          : (tokensAnim ? tokensFps
                                        : (interactive ? 60 : (st.approvalActive ? 30 : 10)));
    if (screenOff || dimmedForNap) targetFps = 5;
    else if (aodActive) targetFps = 4;   // 只在分钟/位移/状态变化时真画
    int64_t frameUs = 1000000LL / targetFps;

    const int64_t drawStartUs = esp_timer_get_time();
    const uint32_t framesBefore = fpsFrames;
    if (!screenOff && !dimmedForNap && canvas) {
      if (needFull) {
        int64_t t0 = esp_timer_get_time();
        if (aodActive) {
          pageHomeDrawAod(*canvas, st, nowMs);
        } else {
          nav.draw(*canvas, st, cfg, frame, nowMs);
        }
        hwDisplayPush();
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needFull = false;
        needRankList = false;
        needTokensSettle = false;
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
      } else if (tokensAnim || (tokensPage && needTokensSettle)) {
        int64_t t0 = esp_timer_get_time();
        pageTokensDrawStage(*canvas, st, cfg, nowMs);
        hwDisplayPushRect(0, 194, UI_W, 88);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needTokensSettle = false;
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
    if (fpsFrames != framesBefore) {
      uiBusyUs += (uint32_t)(esp_timer_get_time() - drawStartUs);
      lastDrawMs = nowMs;
      drawCount++;
    }

    int64_t nowUs = esp_timer_get_time();
    int64_t elapsedUs = nowUs - fpsWindowStartUs;
    if (elapsedUs >= 1000000) {
      curFps = (float)fpsFrames * 1000000.0f / (float)elapsedUs;
      // 只有"真在动"的窗口才更新 drawFps；静帧保留上次值，别再显示 0fps。
      if (fpsFrames >= 8) drawFps = curFps;
      if (animPage && !screenOff && !dimmedForNap && curFps > 5.0f) {
        lastHomeFps = curFps;
      }
      if (elapsedUs > 0) {
        const uint64_t win = (uint64_t)elapsedUs;
        const uint32_t uiPct = (uint32_t)((uint64_t)uiBusyUs * 100u / win);
        const uint32_t decPct = (uint32_t)((uint64_t)decBusyUs * 100u / win);
        uiBusyPct = (uint8_t)(uiPct > 100 ? 100 : uiPct);
        decBusyPct = (uint8_t)(decPct > 100 ? 100 : decPct);
      }
      uiBusyUs = 0;
      decBusyUs = 0;
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
