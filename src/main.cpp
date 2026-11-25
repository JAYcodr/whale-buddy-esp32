#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "anim/nva.h"
#include "audio/sfx.h"
#include "hw/audio.h"
#include "hw/bus.h"
#include "hw/hw.h"
#include "model/settings.h"
#include "model/state.h"
#include "net/net.h"
#include "ui/canvas.h"
#include "ui/easing.h"
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
volatile bool    s_pendingInteract = false;  // 串口 tap/page 也算人动了一下
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
            Serial.printf("fails=qin=%lu qaudio=%lu qnet=%lu fs=%lu nvs=%lu\n",
                          (unsigned long)hwBusInputDropCount(),
                          (unsigned long)hwAudioQueueDropCount(),
                          (unsigned long)netPublishQueueDropCount(),
                          (unsigned long)hwAudioFsFailCount(),
                          (unsigned long)settingsNvsFailCount());
          } else if (strcmp(cmd, "bat") == 0) {
            // Machine-readable power dump for the real-hardware acceptance run.
            HwBattery b = hwBattery();
            BatteryCal cal = hwBatteryCal();
            PowerWatermark wm = hwPowerWatermark();
            const unsigned long durSec = (wm.durMs + 500u) / 1000u;
            Serial.printf("{\"bat\":{\"pct\":%d,\"mv\":%d,\"stage\":%u,\"chg\":%u,"
                          "\"limited\":%s,\"tempCap\":%s,\"usb\":%s,\"vbusLim\":%s,"
                          "\"tmp\":%d,\"gauge\":%d,\"raw\":%u,\"compMv\":%u,"
                          "\"effMa\":%u,\"bootMv\":%u,\"seed\":%d,\"pwrLost\":%s,"
                          "\"mohm\":%u,\"mah\":%u,\"vbusMv\":%u,\"vsysMv\":%u,"
                          "\"thermReg\":%s,\"onSrc\":%u,\"offSrc\":%u,"
                          "\"vbusLimMv\":%u,\"chgLed\":\"%s\",\"wdt\":%u,"
                          "\"wm\":{\"dur\":%lu,\"sp\":%u,\"vmin\":%u,\"vmax\":%u,"
                          "\"vsmin\":%u,\"vbmin\":%u,\"tmax\":%d,\"vlimS\":%u,"
                          "\"tregS\":%u,\"tcapS\":%u,\"scrS\":%u,\"aodS\":%u}}}\n",
                          b.pct, b.mV, (unsigned)b.stage, (unsigned)b.chgMa,
                          b.chargeLimited ? "true" : "false",
                          b.chargeTempCapped ? "true" : "false",
                          b.usbPresent ? "true" : "false",
                          b.vbusLimited ? "true" : "false",
                          b.tempC, (int)b.gaugePct,
                          (unsigned)b.rawPct, (unsigned)b.compMv,
                          (unsigned)b.effMa, (unsigned)b.bootMv,
                          (int)b.seedPct, b.seedPowerLost ? "true" : "false",
                          (unsigned)cal.chgMohm, (unsigned)cal.designMah,
                          (unsigned)b.vbusMv, (unsigned)b.vsysMv,
                          b.thermReg ? "true" : "false",
                          (unsigned)b.pwrOnSrc, (unsigned)b.pwrOffSrc,
                          (unsigned)b.vbusVolLimMv,
                          b.chgLedOff ? "off" : "auto",
                          b.wdtEnabled ? (unsigned)b.wdtTimeoutSec : 0u,
                          durSec, (unsigned)wm.startPct,
                          (unsigned)wm.vbatMinMv, (unsigned)wm.vbatMaxMv,
                          (unsigned)wm.vsysMinMv, (unsigned)wm.vbusMinMv,
                          (int)wm.tmpMaxC,
                          (unsigned)powerMsToClampedSec(wm.vbusLimMs),
                          (unsigned)powerMsToClampedSec(wm.thermRegMs),
                          (unsigned)powerMsToClampedSec(wm.tempCapMs),
                          (unsigned)powerMsToClampedSec(wm.screenOnMs),
                          (unsigned)powerMsToClampedSec(wm.aodMs));
          } else if (strcmp(cmd, "batcal") == 0) {
            if ((doc["reset"] | 0) != 0) hwPowerResetCal();
            BatteryCal cal = hwBatteryCal();
            Serial.printf("{\"batcal\":{\"chgMohm\":%u,\"designMah\":%u,\"cvTauMs\":%lu,"
                          "\"fullRestMv\":%u,\"lowAnchorMv\":%u,\"limitPct\":%u,"
                          "\"rSamples\":%u,\"capSamples\":%u,\"cvSamples\":%u}}\n",
                          (unsigned)cal.chgMohm, (unsigned)cal.designMah,
                          (unsigned long)cal.cvTauMs,
                          (unsigned)cal.fullRestMv, (unsigned)cal.lowAnchorMv,
                          (unsigned)settingsGet().chargeLimitPct,
                          (unsigned)cal.rSamples, (unsigned)cal.capSamples,
                          (unsigned)cal.cvSamples);
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
            else if (strcmp(p, "settings_adv") == 0 || strcmp(p, "settings_sound") == 0) { pageSettingsSetTab(2); nav.setPage(UiPage::SETTINGS); needFull = true; }
            else if (strcmp(p, "settings_power") == 0 || strcmp(p, "settings_batt") == 0) { pageSettingsSetTab(3); nav.setPage(UiPage::SETTINGS); needFull = true; }
            else if (strcmp(p, "settings_display") == 0 || strcmp(p, "settings_aod") == 0) { pageSettingsSetTab(1); nav.setPage(UiPage::SETTINGS); needFull = true; }
            else if (strcmp(p, "tokens") == 0)   { pageTokensOnEnter(millis()); nav.setPage(UiPage::TOKENS); needFull = true; }
            else if (strcmp(p, "chat") == 0)     { nav.setPage(UiPage::CHAT); needFull = true; }
            s_pendingInteract = true;
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
            netSetStatIntervalMs((uint32_t)(doc["ms"] | 0));
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
  bool needSettingsList = false;
  bool wasSettingsAnim = false;
  bool wasApprovalActive = false;
  bool wasTokensAnim = false;
  bool needTokensSettle = false;
  bool needTokensHud = false;

  // v0.5.0：AOD + 观测
  bool     aodActive = false;
  bool     aodForced = false;       // 串口 aodforce 打开的验收模式
  uint32_t aodEnterMs = 0;
  NvaAnimKey aodPoseApplied = NvaAnimKey::COUNT;
  bool     swallowTap = false;      // 唤醒 AOD 的那一击不派发
  enum class AodFade : uint8_t { NONE, ENTER_DIM, ENTER_RISE, EXIT_DIM, EXIT_RISE };
  constexpr uint32_t kAodDimMs       = 250u;
  constexpr uint32_t kAodEnterRiseMs = 250u;
  constexpr uint32_t kAodExitRiseMs  = 180u;
  AodFade  aodFade        = AodFade::NONE;
  uint32_t aodFadeStartMs = 0;
  uint8_t  aodFadeFromRaw = 0;

  struct BrightCache {
    uint8_t  applied    = 255;
    uint16_t appliedRaw = 256;
    void invalidate() { applied = 255; appliedRaw = 256; }
  } bright;
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
  bool    settingsGrab = false;   // 设置页里按下的那个滑块
  bool    chatHoldActive = false; // 命令方块页按住说话手势锁
  enum class PressPhase : uint8_t { NONE, FADE_IN, HELD, FADE_OUT };
  constexpr uint32_t kPressInMs     = 60u;
  constexpr uint32_t kPressOutMs    = 120u;
  constexpr uint8_t  kPressMaxAlpha = 46u;  // +18% 亮度反馈
  PressPhase pressPhase     = PressPhase::NONE;
  uint32_t   pressStartMs   = 0;
  uint8_t    pressAlpha     = 0;
  uint8_t    pressFadeFrom  = 0;
  bool       pressNeedsPush = false;
  ui::Rect   pressedRect{ 0, 0, 0, 0 };
  uint16_t*  pressCleanBuf  = nullptr;
  size_t     pressCleanCap  = 0;
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
  uint32_t lastRecGen = 0;
  uint32_t lastChatGen = 0;
  uint32_t lastBoardGen = 0;
  uint32_t lastRecMeterDrawMs = 0;
  uint32_t lastChatSecDrawMs = 0;
  uint8_t  lastRecLiveLevel = 255;
  uint32_t lastRecLiveDurBucket = 0;
  uint16_t lastRunCount = 0;

  novaStateWithLock([](NovaState& s) {
    s.recGainDb = settingsGet().micGainDb;
  });
  hwBusRequestMicGain(settingsGet().micGainDb);
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
  bool     lastUsbPresent = false;
  uint8_t  lastBattStage = 255;
  uint16_t lastDetailMv = 0;
  uint16_t lastDetailChgMa = 0;
  bool     lastDetailTempCap = false;
  uint32_t lastDetailDrawMs = 0;
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

  // 亮度去重：只有有效寄存值变化才写 QSPI。
  auto applyBrightRaw = [&](uint8_t raw, uint8_t effPct) {
    if (raw != bright.appliedRaw) {
      bright.appliedRaw = raw;
      hwDisplayBrightnessRaw(raw);
    }
    bright.applied = effPct;
  };

  auto clipToScreen = [](const ui::Rect& r) -> ui::Rect {
    int16_t x0 = r.x < 0 ? 0 : r.x;
    int16_t y0 = r.y < 0 ? 0 : r.y;
    int16_t x1 = (r.x + r.w) > UI_W ? UI_W : (int16_t)(r.x + r.w);
    int16_t y1 = (r.y + r.h) > UI_H ? UI_H : (int16_t)(r.y + r.h);
    return { x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
  };

  auto rectsOverlap = [](const ui::Rect& a, const ui::Rect& b) -> bool {
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 &&
           a.x < (b.x + b.w) && (a.x + a.w) > b.x &&
           a.y < (b.y + b.h) && (a.y + a.h) > b.y;
  };

  auto capturePressClean = [&](const ui::Rect& r) -> bool {
    if (!canvas || r.w <= 0 || r.h <= 0) return false;
    uint16_t* fb = canvas->getFramebuffer();
    if (!fb) return false;
    const size_t needPx = (size_t)r.w * (size_t)r.h;
    if (needPx > pressCleanCap) {
      free(pressCleanBuf);
      pressCleanBuf = (uint16_t*)heap_caps_malloc(needPx * sizeof(uint16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!pressCleanBuf) pressCleanBuf = (uint16_t*)malloc(needPx * sizeof(uint16_t));
      pressCleanCap = pressCleanBuf ? needPx : 0;
    }
    if (!pressCleanBuf) return false;
    for (int16_t row = 0; row < r.h; row++) {
      memcpy(pressCleanBuf + (size_t)row * r.w,
             fb + (size_t)(r.y + row) * UI_W + r.x,
             (size_t)r.w * sizeof(uint16_t));
    }
    return true;
  };

  auto restorePressClean = [&](const ui::Rect& r) {
    if (!canvas || !pressCleanBuf || r.w <= 0 || r.h <= 0) return;
    uint16_t* fb = canvas->getFramebuffer();
    if (!fb) return;
    for (int16_t row = 0; row < r.h; row++) {
      memcpy(fb + (size_t)(r.y + row) * UI_W + r.x,
             pressCleanBuf + (size_t)row * r.w,
             (size_t)r.w * sizeof(uint16_t));
    }
  };

  auto cancelPress = [&]() {
    pressPhase = PressPhase::NONE;
    pressAlpha = 0;
    pressFadeFrom = 0;
    pressNeedsPush = false;
  };

  auto startPressIn = [&](const ui::Rect& rawRect, uint32_t ms) {
    ui::Rect r = clipToScreen(rawRect);
    if (r.w <= 0 || r.h <= 0) return;
    // 若上一块按下高亮还在回落，先恢复其干净区域并推屏，避免残影被新快照吃进底色。
    if (pressPhase == PressPhase::FADE_OUT && pressAlpha > 0) {
      restorePressClean(pressedRect);
      hwDisplayPushRect(pressedRect.x, pressedRect.y, pressedRect.w, pressedRect.h);
    }
    if (!capturePressClean(r)) {
      cancelPress();
      return;
    }
    pressedRect = r;
    pressPhase = PressPhase::FADE_IN;
    pressStartMs = ms;
    pressAlpha = 0;
    pressFadeFrom = 0;
    pressNeedsPush = false;
  };

  auto startPressOut = [&](uint32_t ms) {
    if (pressPhase != PressPhase::FADE_IN && pressPhase != PressPhase::HELD) return;
    if (pressPhase == PressPhase::FADE_IN) {
      float p = ease::outCubic(ease::progress(ms, pressStartMs, kPressInMs));
      pressAlpha = (uint8_t)lroundf((float)kPressMaxAlpha * p);
    }
    if (pressAlpha == 0) {
      cancelPress();
      return;
    }
    pressPhase = PressPhase::FADE_OUT;
    pressFadeFrom = pressAlpha;
    pressStartMs = ms;
    pressNeedsPush = false;
  };

  auto syncPressAfterDraw = [&](const ui::Rect& drawnRect) {
    if (!rectsOverlap(pressedRect, drawnRect)) return;
    if (pressPhase == PressPhase::NONE) {
      pressNeedsPush = false;
      return;
    }
    if (capturePressClean(pressedRect) && pressAlpha > 0) {
      ui::drawPressedOverlay(*canvas, pressedRect, 12, pressAlpha);
    }
    pressNeedsPush = false;
  };

  for (;;) {
    esp_task_wdt_reset();
    uint32_t nowMs = millis();
    pollSerialCommands(nav, needFull, fpsFrames, fpsWindowStartUs, nextFrameAt);

    const NovaSettings& cfg = settingsGet();
    BusSnapshot snap = hwBusSnapshot();
    netSetPowerState(snap.battery.usbPresent);
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
      s.battStage        = snap.battery.stage;
      s.battTempC        = (int8_t)snap.battery.tempC;
      s.chgMa            = snap.battery.chgMa;
      s.chargeLimited    = snap.battery.chargeLimited;
      s.chargeTempCapped = snap.battery.chargeTempCapped;
      s.gaugePct         = snap.battery.gaugePct;
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

    const HwRecSnapshot recSnap = hwAudioRecSnapshot();
    if (recSnap.recording) {
      lastInteractMs = nowMs;
      novaStateWithLock([&](NovaState& s) {
        s.recState = 1;
        s.recLevel = recSnap.liveLevel;
        s.recDurMs = recSnap.durMs;
        s.recPeakDb = recSnap.peakDb;
        s.recRmsDb = recSnap.rmsDb;
        s.recClipPermille = recSnap.clipPermille;
      });
      st.recState = 1;
      st.recLevel = recSnap.liveLevel;
      st.recDurMs = recSnap.durMs;
      st.recPeakDb = recSnap.peakDb;
      st.recRmsDb = recSnap.rmsDb;
      st.recClipPermille = recSnap.clipPermille;
      const uint32_t durBucket = recSnap.durMs / 100u;
      if (!aodActive && !screenOff && !dimmedForNap && !st.approvalActive &&
          nav.currentPage() == UiPage::CHAT &&
          (uint32_t)(nowMs - lastRecMeterDrawMs) >= 83u &&
          (recSnap.liveLevel != lastRecLiveLevel || durBucket != lastRecLiveDurBucket)) {
        lastRecMeterDrawMs = nowMs;
        lastRecLiveLevel = recSnap.liveLevel;
        lastRecLiveDurBucket = durBucket;
        needFull = true;
      }
    } else if (recSnap.clipReady) {
      hwAudioRecConsumeReady();
      lastInteractMs = nowMs;
      novaStateWithLock([&](NovaState& s) {
        s.recHasClip = true;
        s.recClipId = recSnap.clipId;
        s.recDurMs = recSnap.durMs;
        s.recPeakDb = recSnap.peakDb;
        s.recRmsDb = recSnap.rmsDb;
        s.recClipPermille = recSnap.clipPermille;
        s.recState = 2;  // UPLOADING
        s.recUploadPct = 0;
        s.recWaitStartMs = 0;
        s.recErr[0] = '\0';
        s.recGen++;
      });
      novaStateRead(&st);
      if (!netStartMicUpload(recSnap.pcmData, recSnap.sampleCount, recSnap.clipId,
                             recSnap.gainDb, recSnap.durMs, recSnap.peakDb,
                             recSnap.rmsDb, recSnap.clipPermille)) {
        novaStateRead(&st);
      }
      sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
      needFull = true;
    }

    auto setChatToast = [&](const char* msg) {
      novaStateWithLock([&](NovaState& s) {
        novaSafeUtf8Copy(s.boardToast, sizeof(s.boardToast), msg);
        s.boardToastStartMs = nowMs;
        s.boardGen++;
      });
    };

    auto confirmAndSendChatDraft = [&](const char* mode) {
      uint16_t did = 0;
      char dtext[160] = {};
      char targetId[48] = {};
      bool sent = false;
      novaStateWithLock([&](NovaState& s) {
        if (!s.chatDraftActive) return;
        const BoardSession* target = novaBoardFind(s, s.boardTargetId);
        if (!target || target->state == BoardSessState::WAIT) {
          target = novaBoardDefaultTarget(s);
        }
        if (!target) {
          novaSafeUtf8Copy(s.boardToast, sizeof(s.boardToast), "请先选可用会话");
          s.boardToastStartMs = nowMs;
          s.boardGen++;
          return;
        }
        did = s.chatDraftId;
        novaSafeUtf8Copy(dtext, sizeof(dtext), s.chatDraftText);
        novaSafeUtf8Copy(targetId, sizeof(targetId), target->id);
        novaSafeUtf8Copy(s.boardTargetId, sizeof(s.boardTargetId), target->id);
        if (dtext[0]) {
          novaChatAppend(s, true, did, dtext);
          s.chatWaitingReply = true;
          s.recErr[0] = '\0';
          if (s.recState == 5) s.recState = 0;
          s.chatDelivery.kind = ChatDelivKind::SENT;
          s.chatDelivery.id = did;
          s.chatDelivery.queuePos = 1;
          s.chatDelivery.secs = 0;
          s.chatDelivery.updatedMs = nowMs;
          novaSafeUtf8Copy(s.chatDelivery.targetId, sizeof(s.chatDelivery.targetId), target->id);
          novaSafeUtf8Copy(s.chatDelivery.targetName, sizeof(s.chatDelivery.targetName), target->name);
          s.chatDelivery.text[0] = '\0';
          s.boardExpandedId[0] = '\0';
          sent = true;
        }
        s.chatDraftActive = false;
        s.chatDraftText[0] = '\0';
        s.chatGen++;
        s.boardGen++;
      });
      if (sent && dtext[0]) {
        netPublishChatSend(did, dtext, targetId, mode ? mode : "queue");
        sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
      } else {
        sfxPlay(SfxKind::DENY, cfg.soundPct);
      }
      novaStateRead(&st);
      needFull = true;
    };

    const bool chatBusy = (st.recState == 1) || (st.recState == 2) ||
                          (st.recState == 3) || (st.recState == 4) ||
                          st.chatDraftActive || st.chatWaitingReply;
    if (chatBusy) {
      lastInteractMs = nowMs;
    }
    if (st.recState == 3 || st.recState == 4) {
      bool timedOut = false;
      novaStateWithLock([&](NovaState& s) {
        timedOut = novaRecCheckWaitTimeout(s, nowMs);
      });
      if (timedOut) {
        novaStateRead(&st);
        needFull = true;
      }
    }
    if (st.boardToast[0] && nowMs >= st.boardToastStartMs &&
        (uint32_t)(nowMs - st.boardToastStartMs) >= 1500u) {
      novaStateWithLock([](NovaState& s) {
        s.boardToast[0] = '\0';
        s.boardGen++;
      });
      novaStateRead(&st);
    }

    // ── AOD（熄屏待机）状态机 ────────────────────────────────────────
    // 闲置只看端侧输入（触摸/按键/PEK）。宿主的运行会话不参与判定：
    // 一个跑五六个小时的任务也该在 N 分钟后进表盘，不能亮一整天。
    // 进入阈值：电池常开时压到 batSleepSec，否则「30 秒先熄屏、120 秒才想进 AOD」永远轮不到。
    uint32_t aodThresholdMs = aod::idleMsFor(cfg.aodIdleSec);
    if (cfg.aodMode == aod::ALWAYS && !snap.battery.usbPresent) {
      const uint32_t sleepMs =
          (uint32_t)(cfg.batSleepSec >= 10 ? cfg.batSleepSec : 30) * 1000u;
      if (sleepMs < aodThresholdMs) aodThresholdMs = sleepMs;
    }
    auto currentBrightRaw = [&]() -> uint8_t {
      return (bright.appliedRaw <= 255u) ? (uint8_t)bright.appliedRaw
                                         : hwDisplayPctToRaw(cfg.brightPct);
    };
    auto beginAodEnter = [&]() {
      if (screenOff || dimmedForNap) {
        aodActive = true;
        aodEnterMs = nowMs;
        aodFade = AodFade::NONE;
        bright.invalidate();
        needFull = true;
      } else if (!aodActive && aodFade != AodFade::ENTER_DIM) {
        aodFade = AodFade::ENTER_DIM;
        aodFadeStartMs = nowMs;
        aodFadeFromRaw = currentBrightRaw();
      }
    };
    auto beginAodExit = [&](bool clearForced, bool urgent) {
      if (clearForced) aodForced = false;
      if (aodActive) {
        if (urgent || screenOff || dimmedForNap) {
          aodActive = false;
          aodPoseApplied = NvaAnimKey::COUNT;
          needFull = true;
          aodFade = (screenOff || dimmedForNap) ? AodFade::NONE : AodFade::EXIT_RISE;
          aodFadeStartMs = nowMs;
          aodFadeFromRaw = currentBrightRaw();
          if (aodFade == AodFade::NONE) bright.invalidate();
        } else if (aodFade != AodFade::EXIT_DIM) {
          aodFade = AodFade::EXIT_DIM;
          aodFadeStartMs = nowMs;
          aodFadeFromRaw = currentBrightRaw();
        }
      } else if (aodFade == AodFade::ENTER_DIM) {
        aodFade = (screenOff || dimmedForNap) ? AodFade::NONE : AodFade::EXIT_RISE;
        aodFadeStartMs = nowMs;
        aodFadeFromRaw = currentBrightRaw();
      }
    };

    if (s_aodForceReq >= 0) {
      aodForced = (s_aodForceReq == 1);
      s_aodForceReq = -1;
      if (aodForced) {
        beginAodEnter();
      } else {
        lastInteractMs = nowMs;
        beginAodExit(true, false);
      }
    }
    if (aodActive) {
      const bool exitNow = !aodForced &&
          aod::shouldExit(cfg.aodMode, snap.battery.usbPresent, st.approvalActive,
                          st.wifiApMode, screenOff, st.battPct);
      if (exitNow) {
        const bool urgent = st.approvalActive || st.wifiApMode || screenOff;
        beginAodExit(false, urgent);
      }
    } else if (!aodForced && aod::shouldEnter(cfg.aodMode, snap.battery.usbPresent,
                                              st.approvalActive, st.wifiApMode, screenOff,
                                              dimmedForNap,
                                              aod::idleSince(nowMs, lastInteractMs),
                                              aodThresholdMs)) {
      beginAodEnter();
    }
    if (aodFade == AodFade::ENTER_DIM && (st.approvalActive || st.wifiApMode)) {
      beginAodExit(false, true);
    }
    if (aodFade == AodFade::ENTER_DIM || aodFade == AodFade::EXIT_DIM) {
      if (screenOff || dimmedForNap) {
        if (aodFade == AodFade::ENTER_DIM) {
          aodActive = true;
          aodEnterMs = nowMs;
          needFull = true;
        } else {
          aodActive = false;
          aodPoseApplied = NvaAnimKey::COUNT;
          needFull = true;
        }
        aodFade = AodFade::NONE;
        bright.invalidate();
      } else if ((uint32_t)(nowMs - aodFadeStartMs) >= kAodDimMs) {
        applyBrightRaw(0, 0);
        if (aodFade == AodFade::ENTER_DIM) {
          aodActive = true;
          aodEnterMs = nowMs;
          aodFade = AodFade::ENTER_RISE;
        } else {
          aodActive = false;
          aodPoseApplied = NvaAnimKey::COUNT;
          aodFade = AodFade::EXIT_RISE;
        }
        aodFadeStartMs = nowMs;
        aodFadeFromRaw = 0;
        needFull = true;
      }
    }

    // AOD 定格姿势：有任务摆 WORKING、等审批摆 WAITING、其余 SLEEP。
    // 换姿势时重开 0.9s 解码窗口，解出一帧后冻结。
    if (aodActive) {
      const NvaAnimKey want = (st.run > 0) ? NvaAnimKey::WORKING
                            : (st.wait > 0 ? NvaAnimKey::WAITING : NvaAnimKey::SLEEP);
      if (want != aodPoseApplied) {
        aodPoseApplied = want;
        aodEnterMs = nowMs;
        nvaSetState(want, false);
        needFull = true;
      }
    }

    // 观测字段：探针从 stat 读这些，别让它们跟主状态错过一整轮。
    novaStateWithLock([&](NovaState& s) {
      s.month = snap.time.Mo;
      s.day = snap.time.D;
      s.dow = snap.time.dow;
      s.aodActive = aodActive;
      s.pwrProfile = (uint8_t)cfg.powerProfile;
      s.effBright = (bright.applied <= 100) ? bright.applied : cfg.brightPct;
      s.lcdOff = (screenOff || dimmedForNap);
      s.uiBusyPct = uiBusyPct;
      s.decBusyPct = decBusyPct;
      s.drawFps = drawFps;
      s.lastDrawMs = lastDrawMs;
      s.drawCount = drawCount;
      s.txPower = netTxPowerApplied();
    });

    hwPowerSetAodActive(aodActive);
    if (st.mqttConnected) {
      static uint32_t lastPwrDrainMs = 0;
      if ((uint32_t)(nowMs - lastPwrDrainMs) >= 100u) {
        PowerEvent pe;
        if (hwPowerPeekEvent(&pe) && netPublishPowerEvent(pe)) {
          hwPowerAckEvent();
          lastPwrDrainMs = nowMs;
          Serial.printf("[pwr] evt=%s seq=%lu pct=%u mv=%u vb=%u vs=%u mohm=%u\n",
                        battPwrEvtReasonStr(pe.reason), (unsigned long)pe.seq,
                        (unsigned)pe.pct, (unsigned)pe.mv,
                        (unsigned)pe.vbusMv, (unsigned)pe.vsysMv,
                        (unsigned)pe.chgMohm);
        }
      }
    }

    // CPU Dynamic Frequency Scaling (DFS via PLL 240/160/80 MHz, keeping APB >= 80 MHz).
    bool activeDemand = st.approvalActive || st.run > 0 || st.wait > 0 ||
                        chatBusy || ((nowMs - lastInteractMs) < 10000u);
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
        bright.invalidate();   // 面板重开后必须重刷一次
        needFull = true;
      }
      if (dimmedForNap) {
        dimmedForNap = false;
        bright.invalidate();
        needFull = true;
      }
    };

    auto makeSlidePillOpts = [&](UiPage fromP, UiPage toP, TransPillOpts* out) -> const TransPillOpts* {
      const int fromDot = uiPageRingIndex(fromP);
      const int toDot   = uiPageRingIndex(toP);
      if (fromDot < 0 || fromDot >= 6 || toDot < 0 || toDot >= 6 || fromDot == toDot) {
        return nullptr;
      }
      *out = { (int8_t)fromDot, (int8_t)toDot, 6,
               ui::burnOffsetX(st.burnInStep), ui::burnOffsetY(st.burnInStep) };
      return out;
    };

    auto playTransition = [&](TransKind kind, uint32_t ms,
                              const TransPillOpts* pills = nullptr) {
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
      cancelPress();
      applyCpuMhz(240);
      nav.draw(*canvas, st, cfg, frame, nowMs);
      memcpy(to, fb, fbBytes);
      uiAnimating = true;
      transRun(*canvas, from, to, kind, ms, pills);
      uiAnimating = false;
      needFull = false;
      frame++;
      fpsFrames++;
    };

    // Handle approval arrival / dismissal transitions (placed after playTransition).
    if (st.apGen != lastApGen) {
      lastApGen = st.apGen;
      cancelPress();
      if (settingsGrab) {
        settingsGrab = false;
        pageSettingsCancelDrag();
      }
      if (st.approvalActive) {
        if (!snap.faceDown && screenOff) {
          hwDisplaySleep(false);
          screenOff = false;
          bright.invalidate();
        }
        if (!snap.faceDown) {
          sfxPlay(SfxKind::APPROVAL_ARRIVE, cfg.soundPct);
          lastInteractMs = nowMs;
        }
        if (!wasApprovalActive) {
          wasApprovalActive = true;
          if (!screenOff && !dimmedForNap && !snap.faceDown) {
            playTransition(TransKind::POP_IN, 200);
          } else {
            needFull = true;
          }
        } else {
          needFull = true;
        }
      } else if (wasApprovalActive) {
        wasApprovalActive = false;
        playTransition(TransKind::POP_OUT, 170);
      } else {
        needFull = true;
      }
    }

    // Raise WiFi power mode during approval, mic recording/upload/chat, or active interaction.
    bool needLowLatencyNet = st.approvalActive || st.wifiApMode ||
                             chatBusy || ((nowMs - lastInteractMs) < 5000);
    netSetPowerSave(needLowLatencyNet ? NetPsMode::PERFORMANCE : NetPsMode::LOW_POWER);

    auto handleAction = [&](const UiAction& act, int slideDir = 0, int swipeOrigin = -1) {
      if (act.kind == UiAction::NONE) return;
      if (act.kind != UiAction::PET_TAP || act.value == 2) needFull = true;
      switch (act.kind) {
        case UiAction::GO_PAGE: {
          UiPage prevPage = (swipeOrigin >= 0) ? (UiPage)swipeOrigin : nav.currentPage();
          UiPage newPage  = (UiPage)act.value;
          nav.setPage(newPage);
          if (newPage == UiPage::SETTINGS && prevPage != UiPage::SETTINGS) {
            pageSettingsOnEnter();
          } else if (prevPage == UiPage::SETTINGS && newPage != UiPage::SETTINGS) {
            pageSettingsCancelDrag();
          }
          if (newPage == UiPage::TOKENS && prevPage != UiPage::TOKENS) {
            pageTokensOnEnter(nowMs);
          }
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          if (prevPage != UiPage::SETTINGS && newPage == UiPage::SETTINGS) {
            playTransition(TransKind::SHEET_UP, 220);
          } else if (prevPage == UiPage::SETTINGS && newPage != UiPage::SETTINGS) {
            playTransition(TransKind::SHEET_DOWN, 220);
          } else if (slideDir != 0) {
            TransPillOpts pillOpts;
            playTransition(slideDir > 0 ? TransKind::SLIDE_LEFT : TransKind::SLIDE_RIGHT,
                           kSlideTransMs, makeSlidePillOpts(prevPage, newPage, &pillOpts));
          }
          break;
        }
        case UiAction::TEST_SOUND:
          needFull = false;
          if (!sfxSpeakFile("/audio/tap0.adp", cfg.soundPct, st.hour, st.timeValid, cfg.quietHours)) {
            sfxPlay(SfxKind::NEW_EVENT, cfg.soundPct);
          }
          break;
        case UiAction::SYNC_HOST:
          netPublishSync();
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::REPROVISION:
          pageSettingsCancelDrag();
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
          playTransition(TransKind::POP_OUT, 170);
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
          playTransition(TransKind::POP_OUT, 170);
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
          playTransition(TransKind::POP_OUT, 170);
          break;
        case UiAction::RANK_REDRAW:
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          playTransition(TransKind::FADE, 150);
          break;
        case UiAction::RESTORE_DEFAULTS:
          settingsRestoreDefaults();
          bright.invalidate();
          netPublishPrefs();
          needFull = true;
          sfxPlay(SfxKind::SETTING_OK, cfg.soundPct);
          break;
        case UiAction::SETTINGS_REDRAW:
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          if (act.value == 0 || act.value == 1) {
            playTransition(TransKind::FADE, 150);
          } else {
            needFull = false;
            needSettingsList = true;
          }
          break;
        case UiAction::TOKENS_REDRAW:
          needFull = false;
          needTokensHud = true;
          needTokensSettle = true;
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::DOCK_TOGGLE:
          novaStateWithLock([](NovaState& s) { s.dockExpanded = !s.dockExpanded; });
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          needFull = true;
          break;
        case UiAction::CHAT_REC_TOGGLE:
          if (st.recState == 2) {
            needFull = false;
          } else if (st.recState == 1) {
            hwAudioRecStop();
          } else {
            uint8_t g = st.recGainDb ? st.recGainDb : 24;
            if (hwAudioRecStart(g)) {
              const HwRecSnapshot startedRec = hwAudioRecSnapshot();
              novaStateWithLock([&](NovaState& s) {
                s.recState = 1;
                s.recClipId = startedRec.clipId;
                s.recWaitStartMs = 0;
                s.recLevel = 0;
                s.recDurMs = 0;
                s.recErr[0] = '\0';
                s.chatDraftActive = false;
                s.chatDraftText[0] = '\0';
                s.recGen++;
                s.chatGen++;
              });
              novaStateRead(&st);
            }
          }
          break;
        case UiAction::CHAT_GAIN_CYCLE:
          if (st.recState != 1) {
            static constexpr uint8_t kGains[5] = { 18, 24, 30, 36, 42 };
            uint8_t cur = st.recGainDb ? st.recGainDb : 24;
            uint8_t next = 24;
            for (int i = 0; i < 5; i++) {
              if (kGains[i] == cur) { next = kGains[(i + 1) % 5]; break; }
            }
            novaStateWithLock([&](NovaState& s) {
              s.recGainDb = next;
              s.recGen++;
            });
            hwBusRequestMicGain(next);
            sfxPlay(SfxKind::SETTING_OK, cfg.soundPct);
          }
          break;
        case UiAction::CHAT_PLAY_TTS:
          needFull = false;
          if (act.value >= 0 && act.value < (int)st.nChatMsgs && !st.chatMsgs[act.value].isUser) {
            netPublishChatTts(st.chatMsgs[act.value].text);
            sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          }
          break;
        case UiAction::CHAT_DRAFT_CANCEL:
          novaStateWithLock([](NovaState& s) {
            s.chatDraftActive = false;
            s.chatDraftText[0] = '\0';
            s.chatGen++;
            s.boardGen++;
          });
          novaStateRead(&st);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          needFull = true;
          break;
        case UiAction::CHAT_DRAFT_SEND:
          confirmAndSendChatDraft("queue");
          break;
        case UiAction::CHAT_DRAFT_SEND_MODE:
          confirmAndSendChatDraft(act.value == 1 ? "steer" : "queue");
          break;
        case UiAction::CHAT_DRAFT_CYCLE_TARGET:
          novaStateWithLock([](NovaState& s) {
            const BoardSession* next = novaBoardCycleTarget(s, s.boardTargetId);
            if (next) {
              novaSafeUtf8Copy(s.boardTargetId, sizeof(s.boardTargetId), next->id);
              s.boardGen++;
            }
          });
          novaStateRead(&st);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          needFull = true;
          break;
        case UiAction::CHAT_SELECT_SESS: {
          bool waitClicked = false;
          novaStateWithLock([&](NovaState& s) {
            if (act.value < 0 || act.value >= (int)s.nBoardSessions || act.value >= MAX_BOARD_SESSIONS) {
              return;
            }
            const BoardSession& bs = s.boardSessions[act.value];
            if (bs.state == BoardSessState::WAIT) {
              waitClicked = true;
              novaSafeUtf8Copy(s.boardExpandedId, sizeof(s.boardExpandedId), bs.id);
              s.boardGen++;
              return;
            }
            if (strcmp(s.boardTargetId, bs.id) == 0 && strcmp(s.boardExpandedId, bs.id) == 0) {
              s.boardTargetId[0] = '\0';
              s.boardExpandedId[0] = '\0';
            } else {
              novaSafeUtf8Copy(s.boardTargetId, sizeof(s.boardTargetId), bs.id);
              novaSafeUtf8Copy(s.boardExpandedId, sizeof(s.boardExpandedId), bs.id);
              s.chatDelivery.kind = ChatDelivKind::NONE;
            }
            s.boardGen++;
          });
          if (waitClicked && !st.approvalActive) {
            setChatToast("请回电脑端确认此审批");
          }
          novaStateRead(&st);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          needFull = true;
          break;
        }
        case UiAction::CHAT_DELIVERY_DISMISS:
          novaStateWithLock([](NovaState& s) {
            s.chatDelivery.kind = ChatDelivKind::NONE;
            s.boardGen++;
          });
          novaStateRead(&st);
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          needFull = true;
          break;
        case UiAction::CHAT_REPLAY_LAST:
          needFull = false;
          if (act.value == 0 && st.chatDelivery.kind == ChatDelivKind::REPLY && st.chatDelivery.text[0]) {
            netPublishChatTts(st.chatDelivery.text);
            setChatToast("播报中...");
            sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
            needFull = true;
          } else if (st.lastSayText[0]) {
            netPublishChatTts(st.lastSayText);
            setChatToast("播报中...");
            sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
            needFull = true;
          }
          break;
        case UiAction::CHAT_OPEN_APPROVAL:
          if (!st.approvalActive) {
            setChatToast("等待审批同步中...");
            needFull = true;
          }
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          break;
        case UiAction::CHAT_QUICK_SEND:
          break;
        default:
          if (settingsApplyAction(act.kind, act.value)) {
            switch (act.kind) {
              case UiAction::SET_SOUND:
                needFull = false;
                needSettingsList = true;
                netPublishPrefs();
                if (settingsGet().soundPct > 0) {
                  sfxPlay(SfxKind::SETTING_OK, settingsGet().soundPct);
                }
                break;
              case UiAction::SET_TTS:
              case UiAction::SET_QUIET:
              case UiAction::SET_CHOREO:
                needFull = false;
                needSettingsList = true;
                netPublishPrefs();
                sfxPlay(SfxKind::SETTING_OK, cfg.soundPct);
                break;
              case UiAction::SET_BRIGHT:
                needFull = false;
                needSettingsList = true;
                bright.invalidate();   // 交给统一亮度逻辑落地
                sfxPlay(SfxKind::SETTING_OK, cfg.soundPct);
                break;
              case UiAction::SET_AOD_MODE:
              case UiAction::SET_AOD_NIGHT:
                bright.invalidate();
                needFull = true;
                sfxPlay(SfxKind::SETTING_OK, cfg.soundPct);
                break;
              case UiAction::SET_MIC_GAIN:
                needFull = false;
                needSettingsList = true;
                novaStateWithLock([](NovaState& s) {
                  s.recGainDb = settingsGet().micGainDb;
                  s.recGen++;
                });
                hwBusRequestMicGain(settingsGet().micGainDb);
                sfxPlay(SfxKind::SETTING_OK, cfg.soundPct);
                break;
              default:
                sfxPlay(SfxKind::SETTING_OK, cfg.soundPct);
                break;
            }
          }
          break;
      }
    };

    // 串口注入的点击：复用 handleAction，与真实触屏同一条路径。
    if (s_pendingInteract) {
      s_pendingInteract = false;
      lastInteractMs = nowMs;   // 串口调试不该被 AOD 打断
    }

    if (s_pendingTap) {
      const int16_t tx = s_pendingTapX;
      const int16_t ty = s_pendingTapY;
      s_pendingTap = false;
      // 注入的点击也算一次交互：不刷新的话退出 AOD 后下一轮就立刻又判进去，
      // 串口调页面时屏幕会一直停在表盘上。
      lastInteractMs = nowMs;
      if (aodActive || aodFade == AodFade::ENTER_DIM) {
        beginAodExit(true, false);
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
        if (aodActive || aodFade == AodFade::ENTER_DIM) {
          beginAodExit(true, false);
          swallowTap = true;
        }
        wakeScreen();
        tpActive = true;
        tpDraggedV = false;
        settingsGrab = false;
        tpStartX = ev.x;
        tpStartY = ev.y;
        tpLastY  = ev.y;
        if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
          pageRankTouchDown(ev.y, ev.tsMs);
        }
        if (!st.approvalActive && nav.currentPage() == UiPage::SETTINGS) {
          settingsGrab = pageSettingsTouchDown(ev.x, ev.y, ev.tsMs);
          if (settingsGrab) needSettingsList = true;
        }
        if (!st.approvalActive && nav.currentPage() == UiPage::CHAT &&
            !swallowTap && pageChatIsHoldBtn(ev.x, ev.y, st)) {
          cancelPress();
          if (pageChatCanStartHold(st)) {
            uint8_t g = settingsGet().micGainDb ? settingsGet().micGainDb : 24;
            if (hwAudioRecStart(g)) {
              chatHoldActive = true;
              const HwRecSnapshot startedRec = hwAudioRecSnapshot();
              novaStateWithLock([&](NovaState& s) {
                s.recState = 1;
                s.recClipId = startedRec.clipId;
                s.recWaitStartMs = 0;
                s.recLevel = 0;
                s.recDurMs = 0;
                s.recGainDb = g;
                s.recErr[0] = '\0';
                s.boardHoldCancel = false;
                s.chatDraftActive = false;
                s.chatDraftText[0] = '\0';
                s.recGen++;
                s.chatGen++;
              });
              novaStateRead(&st);
              needFull = true;
            }
          }
          continue;
        }
        ui::Rect hitRect{ 0, 0, 0, 0 };
        if (!swallowTap && !dimmedForNap && canvas &&
            nav.pressedRect(ev.x, ev.y, st, cfg, &hitRect)) {
          startPressIn(hitRect, nowMs);
        } else if (pressPhase == PressPhase::FADE_IN || pressPhase == PressPhase::HELD) {
          cancelPress();
        }
      } else if (ev.type == InputEvtType::TOUCH_MOVE && tpActive) {
        wakeScreen();
        if (chatHoldActive) {
          const bool wantCancel = (tpStartY - ev.y) > 55;
          if (wantCancel != st.boardHoldCancel) {
            novaStateWithLock([&](NovaState& s) { s.boardHoldCancel = wantCancel; });
            st.boardHoldCancel = wantCancel;
            needFull = true;
          }
          tpLastY = ev.y;
          continue;
        }
        int16_t adx = ev.x > tpStartX ? (ev.x - tpStartX) : (tpStartX - ev.x);
        int16_t ady = ev.y > tpStartY ? (ev.y - tpStartY) : (tpStartY - ev.y);
        if ((pressPhase == PressPhase::FADE_IN || pressPhase == PressPhase::HELD) &&
            (adx >= 12 || ady >= 12)) {
          startPressOut(nowMs);
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
        if (!st.approvalActive && nav.currentPage() == UiPage::SETTINGS) {
          if (settingsGrab) {
            if (pageSettingsTouchMove(ev.x, ev.y, ev.tsMs)) needSettingsList = true;
          } else {
            if (!tpDraggedV && ady >= 8 && ady > adx) {
              tpDraggedV = true;
              if (pressPhase != PressPhase::NONE) {
                cancelPress();
                needSettingsList = true;
              }
            }
            if (tpDraggedV && pageSettingsScrollTouchMove(ev.y, ev.tsMs)) needSettingsList = true;
          }
        }
        if (!st.approvalActive && nav.currentPage() == UiPage::CHAT &&
            tpStartY >= 90 && tpStartY < (st.chatDraftActive ? 200 : 332)) {
          if (!tpDraggedV && ady >= 10 && ady > adx) {
            tpDraggedV = true;
            cancelPress();
          }
          if (tpDraggedV && pageChatScrollTouchMove(ev.y, ev.tsMs, st)) {
            needFull = true;
          }
        }
        tpLastY = ev.y;
      } else if (ev.type == InputEvtType::TOUCH_UP && tpActive) {
        tpActive = false;
        wakeScreen();
        if (swallowTap) {
          swallowTap = false;
          cancelPress();
          continue;
        }
        if (chatHoldActive) {
          chatHoldActive = false;
          const HwRecSnapshot curRec = hwAudioRecSnapshot();
          const bool cancelled = st.boardHoldCancel;
          novaStateWithLock([](NovaState& s) { s.boardHoldCancel = false; });
          if (curRec.recording) {
            if (cancelled) {
              hwAudioRecCancel();
              novaStateWithLock([&](NovaState& s) {
                s.recState = 0;
                novaSafeUtf8Copy(s.boardToast, sizeof(s.boardToast), "已取消");
                s.boardToastStartMs = nowMs;
                s.recGen++;
                s.boardGen++;
              });
            } else if (curRec.durMs < 350u) {
              hwAudioRecCancel();
              novaStateWithLock([&](NovaState& s) {
                s.recState = 0;
                novaSafeUtf8Copy(s.boardToast, sizeof(s.boardToast), "说得太短了");
                s.boardToastStartMs = nowMs;
                s.recGen++;
                s.boardGen++;
              });
              sfxPlay(SfxKind::DENY, cfg.soundPct);
            } else {
              hwAudioRecStop();
            }
          }
          novaStateRead(&st);
          needFull = true;
          continue;
        }
        if (settingsGrab) {
          cancelPress();
          settingsGrab = false;
          if (!st.approvalActive && nav.currentPage() == UiPage::SETTINGS) {
            UiAction up = pageSettingsTouchUp(ev.tsMs);
            if (up.kind != UiAction::NONE) handleAction(up);
          } else {
            pageSettingsCancelDrag();
          }
          needSettingsList = true;
          continue;
        }
        if (tpDraggedV) {
          tpDraggedV = false;
          if (!st.approvalActive && nav.currentPage() == UiPage::SETTINGS) {
            cancelPress();
            pageSettingsScrollTouchUp(ev.tsMs);
            if (!pageSettingsIsAnimating()) {
              needSettingsList = true;
            }
          } else {
            startPressOut(nowMs);
          }
          if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
            pageRankTouchUp(ev.tsMs, st.nRows);
            if (!pageRankIsFlinging()) {
              needFull = true;
            }
          }
          if (!st.approvalActive && nav.currentPage() == UiPage::CHAT) {
            pageChatScrollTouchUp(ev.tsMs, st);
            if (!pageChatIsFlinging()) {
              needFull = true;
            }
          }
          continue;
        }
        if (!st.approvalActive && nav.currentPage() == UiPage::RANK) {
          pageRankTouchUp(ev.tsMs, st.nRows);
        }
        if (!st.approvalActive && nav.currentPage() == UiPage::SETTINGS) {
          pageSettingsScrollCancel();
        }
        if (!st.approvalActive && nav.currentPage() == UiPage::CHAT) {
          pageChatScrollCancel();
        }
        int16_t dx = ev.x - tpStartX;
        int16_t dy = ev.y - tpStartY;
        int16_t adx = dx < 0 ? -dx : dx;
        int16_t ady = dy < 0 ? -dy : dy;
        UiPage prevPage = nav.currentPage();
        const bool blockSwipe = st.approvalActive ||
                                (prevPage == UiPage::CHAT && st.recState == 1);
        UiAction act = blockSwipe ? UiAction{} : nav.onSwipe(dx, dy);
        if (act.kind != UiAction::NONE) {
          int slideDir = (prevPage == UiPage::SETTINGS && dy >= 60 && dy > adx)
                           ? 0
                           : (dx < 0 ? 1 : -1);
          handleAction(act, slideDir, (int)prevPage);
          if (needFull) cancelPress();
        } else if (!(prevPage == UiPage::SETTINGS && (adx >= 16 || ady >= 16))) {
          act = nav.onTap(ev.x, ev.y, st, cfg);
          handleAction(act);
          if (needFull || (nav.currentPage() == UiPage::SETTINGS && pageSettingsIsScrolling())) {
            cancelPress();
          } else {
            startPressOut(nowMs);
          }
        } else {
          startPressOut(nowMs);
        }
      } else if (ev.type == InputEvtType::KEY1_DOWN) {
        if (aodActive || aodFade == AodFade::ENTER_DIM) {
          beginAodExit(true, false);
        }
        wakeScreen();
        key1DownAt = nowMs;
        key1LongFired = false;
      } else if (ev.type == InputEvtType::KEY1_UP) {
        if (!key1LongFired && key1DownAt != 0) {
          UiPage prev = nav.currentPage();
          nav.goHome(cfg.homePage);
          UiPage next = nav.currentPage();
          if (prev == UiPage::SETTINGS && next != UiPage::SETTINGS) {
            settingsGrab = false;
            pageSettingsCancelDrag();
          }
          sfxPlay(SfxKind::NAV_CHIRP, cfg.soundPct);
          if (next != prev) {
            TransKind kind = (uiPageRingIndex(next) > uiPageRingIndex(prev))
                               ? TransKind::SLIDE_LEFT
                               : TransKind::SLIDE_RIGHT;
            TransPillOpts pillOpts;
            playTransition(kind, kSlideTransMs, makeSlidePillOpts(prev, next, &pillOpts));
          }
        }
        key1DownAt = 0;
      } else if (ev.type == InputEvtType::AXP_SHORT) {
        if (aodActive || aodFade == AodFade::ENTER_DIM) {
          beginAodExit(true, false);
        } else if (screenOff) {
          wakeScreen();
        } else {
          hwDisplaySleep(true);
          screenOff = true;
          aodFade = AodFade::NONE;
        }
      }
    }

    if (snap.key1Down && key1DownAt != 0 && !key1LongFired && (nowMs - key1DownAt >= KEY1_LONG_MS)) {
      key1LongFired = true;
      if (nav.currentPage() != UiPage::SETTINGS) {
        pageSettingsOnEnter();
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
        bright.invalidate();
      } else if (!snap.faceDown && dimmedForNap) {
        dimmedForNap = false;
        bright.invalidate();
        needFull = true;
      }
    }

    // 统一亮度：暗屏 0 / AOD 降档 / 平时用户档 + AOD 进出背光渐变；只在有效值变化时写 QSPI。
    if (!screenOff) {
      uint8_t wantPct = cfg.brightPct;
      if (dimmedForNap) {
        wantPct = 20;   // 老固件的 0 档 = 20%；翻扣时压到最暗而不是全黑
        aodFade = AodFade::NONE;
      } else if (aodActive) {
        wantPct = aod::brightnessPct(cfg.brightPct, cfg.aodNightCap, aod::isNight(st.hour));
      }
      const uint8_t targetRaw = hwDisplayPctToRaw(wantPct);
      if (aodFade == AodFade::ENTER_DIM || aodFade == AodFade::EXIT_DIM) {
        const float p = ease::outCubic(ease::progress(nowMs, aodFadeStartMs, kAodDimMs));
        const uint8_t raw = (uint8_t)lroundf(ease::lerp((float)aodFadeFromRaw, 0.0f, p));
        applyBrightRaw(raw, (uint8_t)((uint16_t)raw * 100u / 255u));
      } else if (aodFade == AodFade::ENTER_RISE || aodFade == AodFade::EXIT_RISE) {
        const uint32_t dur = (aodFade == AodFade::ENTER_RISE) ? kAodEnterRiseMs : kAodExitRiseMs;
        const uint32_t el = (uint32_t)(nowMs - aodFadeStartMs);
        if (el >= dur) {
          aodFade = AodFade::NONE;
          applyBrightRaw(targetRaw, wantPct);
        } else {
          const float p = ease::outCubic((float)el / (float)dur);
          const uint8_t raw = (uint8_t)lroundf(ease::lerp((float)aodFadeFromRaw, (float)targetRaw, p));
          applyBrightRaw(raw, (uint8_t)((uint16_t)raw * 100u / 255u));
        }
      } else {
        applyBrightRaw(targetRaw, wantPct);
      }
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
        aodFade = AodFade::NONE;
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

    // Battery percentage is smoothed and slewed by the estimator, so 1% moves
    // take >=10-25s; keep a 10s rate limit unless crossing color thresholds or
    // 5% buckets, and redraw immediately when USB/stage changes so icons never lag.
    const bool powerStateChanged = (st.usbPresent != lastUsbPresent) ||
                                   (st.battStage != lastBattStage);
    const bool battColorCrossed = ((st.battPct <= 10) != (lastBattPct <= 10)) ||
                                  ((st.battPct <= 20) != (lastBattPct <= 20));
    const bool battChanged = (st.battPct != lastBattPct) &&
                             (battColorCrossed ||
                              (st.battPct / 5u) != (lastBattPct / 5u) ||
                              (nowMs - lastBattDrawMs) >= 10000u);
    const int mvDelta = (st.battMv > lastDetailMv) ? (int)(st.battMv - lastDetailMv)
                                                   : (int)(lastDetailMv - st.battMv);
    const bool detailPowerChanged = (!aodActive && nav.currentPage() == UiPage::DETAIL) &&
                                    ((nowMs - lastDetailDrawMs) >= 2000u) &&
                                    (mvDelta >= 15 || st.chgMa != lastDetailChgMa ||
                                     st.chargeTempCapped != lastDetailTempCap);

    // Detect state / heartbeat / burn-in pixel shift changes requiring a full redraw.
    if (st.stateGen != lastStateGen || st.quotaGen != lastQuotaGen || st.rankGen != lastRankGen ||
        st.choreoGen != lastChoreoGen || st.tokenGen != lastTokenGen ||
        (nav.currentPage() == UiPage::CHAT &&
         (st.recGen != lastRecGen || st.chatGen != lastChatGen || st.boardGen != lastBoardGen)) ||
        st.wifiConnected != lastWifiUp || st.wifiApMode != lastApMode ||
        st.mqttConnected != lastMqttUp || st.minute != lastMinute ||
        battChanged || powerStateChanged || detailPowerChanged ||
        (!aodActive && st.lastMqttRxMs != lastMqttRxSeen) || isStale != lastStaleFlag ||
        staleBucket != lastStaleShownSec || quotaBucket != lastQuotaBucket ||
        burnStep != lastBurnStep || curSleepState != lastSleepState) {
      lastStateGen      = st.stateGen;
      lastQuotaGen      = st.quotaGen;
      lastRankGen       = st.rankGen;
      lastChoreoGen     = st.choreoGen;
      lastTokenGen      = st.tokenGen;
      lastRecGen        = st.recGen;
      lastChatGen       = st.chatGen;
      lastBoardGen      = st.boardGen;
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
      lastUsbPresent    = st.usbPresent;
      lastBattStage     = st.battStage;
      lastDetailMv      = st.battMv;
      lastDetailChgMa   = st.chgMa;
      lastDetailTempCap = st.chargeTempCapped;
      lastDetailDrawMs  = nowMs;
      lastBurnStep      = burnStep;
      lastSleepState    = curSleepState;
      needFull          = true;
    }
    if (!aodActive && !screenOff && !dimmedForNap && !st.approvalActive &&
        nav.currentPage() == UiPage::CHAT && st.recState != 1 && !st.chatDraftActive) {
      bool hasRunning = (st.chatDelivery.kind == ChatDelivKind::RUN);
      for (uint8_t i = 0; !hasRunning && i < st.nBoardSessions && i < MAX_BOARD_SESSIONS; i++) {
        if (st.boardSessions[i].state == BoardSessState::RUN) hasRunning = true;
      }
      if (hasRunning && (uint32_t)(nowMs - lastChatSecDrawMs) >= 1000u) {
        lastChatSecDrawMs = nowMs;
        needFull = true;
      }
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
    if (!st.approvalActive && nav.currentPage() == UiPage::SETTINGS) {
      if (pageSettingsTick(nowMs)) {
        needSettingsList = true;
      }
      bool curSettingsAnim = pageSettingsIsAnimating();
      if (wasSettingsAnim && !curSettingsAnim) {
        needSettingsList = true;
      }
      wasSettingsAnim = curSettingsAnim;
    } else {
      if (pageSettingsIsAnimating()) pageSettingsTick(nowMs);
      wasSettingsAnim = false;
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
      // AOD 期间宠物定格：姿势由上面的 aodPose 逻辑统一设置，这里什么都不做。
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
    // 屏幕不亮就不解码；AOD 进入后先让目标姿势解 0.9s，然后冻结。
    // 熄屏时原来还在 10fps 空转 zlib，纯白耗电（2026-11-23 实测补上）。
    const bool animNeeded =
        !screenOff && (!aodActive || (uint32_t)(nowMs - aodEnterMs) < 900u);
    if (animNeeded) {
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

    if (!st.approvalActive && nav.currentPage() == UiPage::CHAT) {
      if (pageChatTick(nowMs, st)) {
        needFull = true;
      }
    } else if (pageChatIsFlinging()) {
      pageChatScrollCancel();
    }

    // Frame rate target depends on active page & power profile.
    // 异常详情浮层开着时不能走舞台局部重绘：pageHomeDrawStage 只重画宠物那一块，
    // 浮层画完立刻会被动画覆盖（真机实测：宠物和气泡压在浮层上）。
    bool animPage = (!aodActive && !st.approvalActive && !st.wifiApMode &&
                     nav.currentPage() == UiPage::HOME &&
                     !(st.dockExpanded && st.turnError && st.errMsg[0]));
    // AOD 是整屏浮层：和 animPage 一样必须禁掉局部重绘，否则停在鲸群页进表盘时
    // 鱼群仍按 AOD 的 4fps 往表盘上画（2026-11-23 真机实测：表盘中间一排游动的鱼）。
    bool tokensPage = (!aodActive && !st.approvalActive && !st.wifiApMode &&
                       nav.currentPage() == UiPage::TOKENS);
    bool tokensAnim = tokensPage && pageTokensIsAnimating(st, cfg, nowMs);
    if (wasTokensAnim && !tokensAnim && tokensPage) {
      needTokensSettle = true;
    }
    wasTokensAnim = tokensAnim;

    if (screenOff || dimmedForNap || aodActive) {
      cancelPress();
    } else if (pressPhase == PressPhase::FADE_IN) {
      float p = ease::outCubic(ease::progress(nowMs, pressStartMs, kPressInMs));
      uint8_t nextA = (uint8_t)lroundf((float)kPressMaxAlpha * p);
      if (nextA != pressAlpha) {
        pressAlpha = nextA;
        pressNeedsPush = true;
      }
      if ((uint32_t)(nowMs - pressStartMs) >= kPressInMs) {
        pressAlpha = kPressMaxAlpha;
        pressPhase = PressPhase::HELD;
      }
    } else if (pressPhase == PressPhase::FADE_OUT) {
      float p = ease::outCubic(ease::progress(nowMs, pressStartMs, kPressOutMs));
      uint8_t nextA = (uint8_t)lroundf((float)pressFadeFrom * (1.0f - p));
      if (nextA != pressAlpha) {
        pressAlpha = nextA;
        pressNeedsPush = true;
      }
      if ((uint32_t)(nowMs - pressStartMs) >= kPressOutMs) {
        pressAlpha = 0;
        pressNeedsPush = true;
        pressPhase = PressPhase::NONE;
      }
    }

    bool pressAnimating = (pressPhase == PressPhase::FADE_IN || pressPhase == PressPhase::FADE_OUT);
    bool interactive = tpActive || pageRankIsFlinging() || pageSettingsIsAnimating() ||
                       pageChatIsFlinging() || pressAnimating || (aodFade != AodFade::NONE) || uiAnimating;
    // TOKENS 页一帧要搬 63 KB（368×88）@ 实测 2.7 MB/s ≈ 23 ms，喂不到 60；
    // 鱼又是 ±2.6px / 4s 的慢速微动，30 fps 和 60 fps 肉眼没差别，锁 30 换稳定。
    const uint8_t baseFps = settingsEffectiveFps(snap.battery.usbPresent);
    const uint8_t tokensFps = baseFps > 30 ? 30 : baseFps;
    uint8_t targetFps = animPage
                          ? baseFps
                          : (tokensAnim ? tokensFps
                                        : (interactive ? 60 : (st.approvalActive ? 30 : 10)));
    if (screenOff || dimmedForNap) targetFps = 5;
    else if (aodActive && aodFade == AodFade::NONE) targetFps = 4;   // 只在分钟/位移/状态变化时真画
    int64_t frameUs = 1000000LL / targetFps;

    const int64_t drawStartUs = esp_timer_get_time();
    const uint32_t framesBefore = fpsFrames;
    if (!screenOff && !dimmedForNap && canvas) {
      if (needFull) {
        int64_t t0 = esp_timer_get_time();
        if (aodActive) {
          pageHomeDrawAod(*canvas, st, nowMs);
          cancelPress();
        } else {
          nav.draw(*canvas, st, cfg, frame, nowMs);
          if (pressPhase == PressPhase::FADE_OUT || pressPhase == PressPhase::NONE) {
            cancelPress();
          } else {
            syncPressAfterDraw({ 0, 0, UI_W, UI_H });
          }
        }
        hwDisplayPush();
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needFull = false;
        needRankList = false;
        needSettingsList = false;
        needTokensSettle = false;
        needTokensHud = false;
        frame++;
        fpsFrames++;
      } else if (needSettingsList && !aodActive && !st.approvalActive && nav.currentPage() == UiPage::SETTINGS) {
        int64_t t0 = esp_timer_get_time();
        pageSettingsDrawList(*canvas, st, cfg);
        const ui::Rect sr = pageSettingsListRect();
        if (pageSettingsIsScrolling()) cancelPress();
        else syncPressAfterDraw(sr);
        hwDisplayPushRect(sr.x, sr.y, sr.w, sr.h);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needSettingsList = false;
        frame++;
        fpsFrames++;
      } else if (needRankList && !aodActive && !st.approvalActive && nav.currentPage() == UiPage::RANK) {
        int64_t t0 = esp_timer_get_time();
        pageRankDrawList(*canvas, st);
        const ui::Rect rr = pageRankListRect();
        syncPressAfterDraw(rr);
        hwDisplayPushRect(rr.x, rr.y, rr.w, rr.h);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needRankList = false;
        frame++;
        fpsFrames++;
      } else if (animPage) {
        int16_t dx = 0, dy = 0, dw = 0, dh = 0;
        int64_t t0 = esp_timer_get_time();
        pageHomeDrawStage(*canvas, st, cfg, frame, &dx, &dy, &dw, &dh);
        syncPressAfterDraw({ dx, dy, dw, dh });
        hwDisplayPushRect(dx, dy, dw, dh);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        frame++;
        fpsFrames++;
      } else if (tokensAnim || (tokensPage && needTokensSettle)) {
        int64_t t0 = esp_timer_get_time();
        if (needTokensHud) {
          pageTokensDrawHud(*canvas, st);
          hwDisplayPushRect(0, 62, UI_W, 92);
          hwDisplayPushRect(0, 414, UI_W, UI_H - 414);
          needTokensHud = false;
        }
        pageTokensDrawStage(*canvas, st, cfg, nowMs);
        const ui::Rect tr = pageTokensStageRect();
        syncPressAfterDraw(tr);
        hwDisplayPushRect(tr.x, tr.y, tr.w, tr.h);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        needTokensSettle = false;
        frame++;
        fpsFrames++;
      } else if (st.approvalActive) {
        int64_t t0 = esp_timer_get_time();
        pageApprovalDrawBar(*canvas, st, nowMs);
        const ui::Rect ar = pageApprovalBarRect();
        syncPressAfterDraw(ar);
        hwDisplayPushRect(ar.x, ar.y, ar.w, ar.h);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        frame++;
        fpsFrames++;
      }
      if (pressNeedsPush) {
        int64_t t0 = esp_timer_get_time();
        restorePressClean(pressedRect);
        if (pressAlpha > 0) {
          ui::drawPressedOverlay(*canvas, pressedRect, 12, pressAlpha);
        }
        hwDisplayPushRect(pressedRect.x, pressedRect.y, pressedRect.w, pressedRect.h);
        lastPushUs = (uint32_t)(esp_timer_get_time() - t0);
        pressNeedsPush = false;
        if (fpsFrames == framesBefore) {
          frame++;
          fpsFrames++;
        }
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
  hwPowerTick(millis());
  hwDisplayBrightness(settingsGet().brightPct);

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char devId[16];
  snprintf(devId, sizeof(devId), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  novaStateInit(devId, NOVA_FW_VER);
  {
    const HwBattery bootBat = hwBattery();
    novaStateWithLock([&](NovaState& st) {
      st.pwrOnSrc  = bootBat.pwrOnSrc;
      st.pwrOffSrc = bootBat.pwrOffSrc;
    });
  }
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
