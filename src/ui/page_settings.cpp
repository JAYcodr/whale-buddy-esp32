#include "ui/pages.h"
#include "ui/theme.h"
#include <Arduino.h>
#include <stdio.h>

namespace {

uint8_t s_tab = 0;  // 0 = 基础偏好, 1 = 进阶策略
bool s_reprovConfirm = false;
uint32_t s_reprovAtMs = 0;

constexpr uint32_t REPROV_CONFIRM_TIMEOUT_MS = 3000u;

constexpr ui::Rect HDR_BACK { 14,  12, 196, 36 };
constexpr ui::Rect TAB_SEG  { 186, 13, 162, 34 };
constexpr ui::Rect TAB_BASIC{ 186, 13, 54,  34 };
constexpr ui::Rect TAB_ADV  { 240, 13, 54,  34 };
constexpr ui::Rect TAB_PWR  { 294, 13, 54,  34 };
constexpr const char* TAB_LABELS[3] = { "基础", "进阶", "电源" };

// Tab 0: 基础偏好
constexpr ui::Rect HOME_SEG{ 34, 84, 300, 34 };
constexpr ui::Rect HOME_BTNS[3] = {
  { 34,  84, 100, 34 },
  { 134, 84, 100, 34 },
  { 234, 84, 100, 34 },
};
constexpr const char* HOME_LABELS[3] = { "宠物", "订阅", "榜单" };

constexpr ui::Rect PWR_SEG{ 34, 164, 300, 34 };
constexpr ui::Rect PWR_BTNS[3] = {
  { 34,  164, 100, 34 },
  { 134, 164, 100, 34 },
  { 234, 164, 100, 34 },
};
constexpr const char* PWR_LABELS[3] = { "自适应", "长续航", "灵动" };

constexpr ui::Rect SND_BAR{ 34, 246, 300, 36 };
constexpr ui::Rect SND_STEPS[5] = {
  { 34,  246, 60, 36 },
  { 94,  246, 60, 36 },
  { 154, 246, 60, 36 },
  { 214, 246, 60, 36 },
  { 274, 246, 60, 36 },
};
constexpr uint8_t SND_VALUES[5] = { 0, 25, 50, 75, 100 };

constexpr ui::Rect BRT_BAR{ 34, 326, 300, 38 };
constexpr ui::Rect BRT_STEPS[5] = {
  { 34,  326, 60, 38 },
  { 94,  326, 60, 38 },
  { 154, 326, 60, 38 },
  { 214, 326, 60, 38 },
  { 274, 326, 60, 38 },
};
constexpr const char* BRT_LABELS[5] = { "20%", "40%", "60%", "80%", "100%" };

// Tab 1: 进阶策略 (hit rects keep >= 34px height around 52x30 visual switches)
constexpr ui::Rect BTN_TTS_TOGGLE   { 28,  56, 312, 44 };
constexpr ui::Rect SW_TTS_VISUAL    { 282, 63, 52,  30 };
constexpr ui::Rect BTN_QUIET_TOGGLE { 28, 108, 312, 44 };
constexpr ui::Rect SW_QUIET_VISUAL  { 282, 115, 52, 30 };

constexpr ui::Rect SLEEP_SEG{ 34, 196, 300, 34 };
constexpr ui::Rect SLEEP_BTNS[4] = {
  { 34,  196, 75, 34 },
  { 109, 196, 75, 34 },
  { 184, 196, 75, 34 },
  { 259, 196, 75, 34 },
};
constexpr uint16_t SLEEP_SECS[4] = { 15, 30, 60, 120 };
constexpr const char* SLEEP_LABELS[4] = { "15s", "30s", "60s", "2m" };

constexpr ui::Rect BTN_CHOREO_TOGGLE{ 28, 251, 312, 44 };
constexpr ui::Rect SW_CHOREO_VISUAL { 282, 258, 52, 30 };

constexpr ui::Rect BTN_TEST_SND{ 24,  386, 162, 44 };
constexpr ui::Rect BTN_REPROV  { 24,  386, 162, 44 };
constexpr ui::Rect BTN_BACK    { 198, 386, 146, 44 };

// Tab 2: power / charge policy
constexpr ui::Rect CHG_LIM_SEG{ 34, 86, 300, 34 };
constexpr ui::Rect CHG_LIM_BTNS[3] = {
  { 34,  86, 100, 34 },
  { 134, 86, 100, 34 },
  { 234, 86, 100, 34 },
};
constexpr const char* CHG_LIM_LABELS[3] = { "80%", "90%", "100%" };
constexpr uint8_t CHG_LIM_VALS[3] = { 80, 90, 100 };

constexpr ui::Rect CHG_SPD_SEG{ 34, 192, 300, 34 };
constexpr ui::Rect CHG_SPD_BTNS[3] = {
  { 34,  192, 100, 34 },
  { 134, 192, 100, 34 },
  { 234, 192, 100, 34 },
};
constexpr const char* CHG_SPD_LABELS[3] = { "温和", "自动", "快速" };

constexpr ui::Rect BTN_PWR_BACK{ 24, 386, 320, 44 };

int soundStepIndex(uint8_t pct) {
  if (pct == 0) return 0;
  if (pct <= 35) return 1;
  if (pct <= 60) return 2;
  if (pct <= 85) return 3;
  return 4;
}

int sleepStepIndex(uint16_t sec) {
  if (sec <= 20) return 0;
  if (sec <= 45) return 1;
  if (sec <= 90) return 2;
  return 3;
}

const char* powerStageLabel(const NovaState& st, const NovaSettings& cfg) {
  switch ((BattStage)st.battStage) {
    case BattStage::PRE:       return "涓流预充";
    case BattStage::CC:
    case BattStage::CV:        return "充电中";
    case BattStage::DONE:      return "已充满";
    case BattStage::LIMITED:   return (st.battPct + 1 >= cfg.chargeLimitPct) ? "保养停充" : "高温停充";
    case BattStage::USB_IDLE:  return "USB 供电";
    case BattStage::DISCHARGE: return "电池供电";
    default:                   return "未接电池";
  }
}

// aafont_data.cpp lacks U+2039 ('‹'); draw a crisp 2px vector chevron instead.
void drawHeaderBackChevron(NovaCanvas& c) {
  c.drawLine(29, 23, 22, 30, ui::COL_ACCENT);
  c.drawLine(22, 30, 29, 37, ui::COL_ACCENT);
  c.drawLine(30, 23, 23, 30, ui::COL_ACCENT);
  c.drawLine(23, 30, 30, 37, ui::COL_ACCENT);
}

}  // namespace

void pageSettingsSetTab(uint8_t tab) {
  s_tab = (tab <= 2) ? tab : 0;
  s_reprovConfirm = false;
}

bool pageSettingsTick(uint32_t nowMs) {
  if (s_reprovConfirm && (nowMs - s_reprovAtMs >= REPROV_CONFIRM_TIMEOUT_MS)) {
    s_reprovConfirm = false;
    return true;
  }
  return false;
}

void pageSettingsDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg) {
  c.fillScreen(ui::COL_BG);

  drawHeaderBackChevron(c);
  ui::drawText(c, ui::fontCjk16(), 38, 37, "系统与偏好设置", ui::COL_TEXT, ui::COL_BG);

  // Header tab segmented pill: 基础 / 进阶 / 电源
  ui::drawSegmented(c, TAB_SEG, TAB_LABELS, 3, s_tab, ui::COL_ACCENT, ui::fontCjk14());

  if (s_tab == 0) {
    // Card 1: Default home page
    ui::drawCard(c, 20, 52, 328, 74, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawSectionLabel(c, 34, 74, "默认首页（开机 / Key1 返回）");
    ui::drawSegmented(c, HOME_SEG, HOME_LABELS, 3, (int)cfg.homePage,
                      ui::COL_ACCENT, ui::fontCjk14());

    // Card 2: Power & FPS profile
    ui::drawCard(c, 20, 132, 328, 74, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawSectionLabel(c, 34, 154, "功耗与刷新策略");
    ui::drawSegmented(c, PWR_SEG, PWR_LABELS, 3, (int)cfg.powerProfile,
                      ui::COL_MINT, ui::fontCjk14());

    // Card 3: Sound volume & screen brightness
    ui::drawCard(c, 20, 212, 328, 166, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawText(c, ui::fontCjk14(), 34, 236, "语音与提示音量", ui::COL_TEXT, ui::COL_CARD);
    char sndTag[16];
    if (cfg.soundPct == 0) snprintf(sndTag, sizeof(sndTag), "已静音");
    else snprintf(sndTag, sizeof(sndTag), "%u%%", cfg.soundPct);
    ui::drawTextRight(c, ui::fontCjk14(), 334, 236, sndTag,
                      cfg.soundPct == 0 ? ui::COL_RED : ui::COL_ACCENT, ui::COL_CARD);

    int sndIdx = soundStepIndex(cfg.soundPct);
    ui::drawLevelBar(c, SND_BAR, 5, sndIdx, ui::COL_ACCENT, true);

    c.drawFastHLine(34, 292, 300, rgb565(20, 32, 52));
    ui::drawText(c, ui::fontCjk14(), 34, 314, "屏幕亮度", ui::COL_TEXT, ui::COL_CARD);
    uint8_t brtIdx = (cfg.brightLevel > 4) ? 4 : cfg.brightLevel;
    ui::drawTextRight(c, ui::fontAscii14(), 334, 314, BRT_LABELS[brtIdx],
                      ui::COL_AMBER, ui::COL_CARD);
    ui::drawLevelBar(c, BRT_BAR, 5, brtIdx, ui::COL_AMBER, false);

    ui::drawButton(c, BTN_TEST_SND, "试听语音", false, ui::COL_MINT, ui::fontCjk14());
  } else if (s_tab == 1) {
    // Card 1: Voice & Quiet Hours
    ui::drawCard(c, 20, 52, 328, 104, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawText(c, ui::fontCjk14(), 34, 84, "宿主 TTS 语音播报", ui::COL_TEXT, ui::COL_CARD);
    ui::drawSwitch(c, SW_TTS_VISUAL, cfg.ttsEnabled, ui::COL_ACCENT);

    c.drawFastHLine(34, 104, 300, rgb565(20, 32, 52));
    ui::drawText(c, ui::fontCjk14(), 34, 136, "夜间静音 (23-08点)", ui::COL_TEXT, ui::COL_CARD);
    ui::drawSwitch(c, SW_QUIET_VISUAL, cfg.quietHours, ui::COL_ACCENT);

    // Card 2: Battery sleep timeout & AI choreography
    ui::drawCard(c, 20, 164, 328, 138, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawSectionLabel(c, 34, 186, "电池自动息屏时长");
    int slpIdx = sleepStepIndex(cfg.batSleepSec);
    ui::drawSegmented(c, SLEEP_SEG, SLEEP_LABELS, 4, slpIdx,
                      ui::COL_MINT, ui::fontAscii14());

    c.drawFastHLine(34, 244, 300, rgb565(20, 32, 52));
    ui::drawText(c, ui::fontCjk14(), 34, 270, "AI 自动编排剧本", ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), 34, 291,
                 cfg.choreoEnabled ? "AI 导演" : "仅本地", ui::COL_MUTED, ui::COL_CARD);
    ui::drawSwitch(c, SW_CHOREO_VISUAL, cfg.choreoEnabled, ui::COL_ACCENT);

    // Card 3: MQTT Broker & Firmware Version
    ui::drawCard(c, 20, 310, 328, 66, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawSectionLabel(c, 34, 334, "MQTT Broker");
    char verTag[16];
    snprintf(verTag, sizeof(verTag), "v%s", NOVA_FW_VER);
    ui::drawTextRight(c, ui::fontAscii14(), 334, 334, verTag, ui::COL_MUTED, ui::COL_CARD);

    char rawBroker[64], brokerBuf[64];
    snprintf(rawBroker, sizeof(rawBroker), "%s:%u", cfg.brokerHost, (unsigned)cfg.brokerPort);
    ui::fitUtf8Line(c, ui::fontAscii14(), rawBroker, 290, brokerBuf, sizeof(brokerBuf));
    ui::drawText(c, ui::fontAscii14(), 34, 360, brokerBuf, ui::COL_ACCENT, ui::COL_CARD);

    if (s_reprovConfirm) {
      ui::aaFillRoundRect(c, BTN_REPROV.x, BTN_REPROV.y, BTN_REPROV.w, BTN_REPROV.h,
                          12, ui::COL_RED, ui::COL_RED);
      ui::drawTextCenteredInRect(c, ui::fontCjk14(), BTN_REPROV, "再点一次确认",
                                 ui::COL_BG, ui::COL_RED);
    } else {
      const uint16_t reprovBg = rgb565(40, 28, 12);
      ui::aaFillRoundRect(c, BTN_REPROV.x, BTN_REPROV.y, BTN_REPROV.w, BTN_REPROV.h,
                          12, reprovBg, ui::COL_AMBER);
      ui::drawTextCenteredInRect(c, ui::fontCjk14(), BTN_REPROV, "重新配网 (AP)",
                                 ui::COL_AMBER, reprovBg);
    }
  } else {
    // Card 1: charge limit (battery care)
    ui::drawCard(c, 20, 52, 328, 98, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawSectionLabel(c, 34, 74, "充电上限（电池保养）");
    const uint8_t lim = cfg.chargeLimitPct;
    const int limIdx = (lim <= 80) ? 0 : (lim >= 100 ? 2 : 1);
    ui::drawSegmented(c, CHG_LIM_SEG, CHG_LIM_LABELS, 3, limIdx,
                      ui::COL_MINT, ui::fontAscii14());
    char cap[64];
    snprintf(cap, sizeof(cap), "到 %u%% 自动停充 · 拔插后重充", (unsigned)lim);
    ui::drawText(c, ui::fontCjk14(), 34, 140, cap, ui::COL_MUTED, ui::COL_CARD);

    // Card 2: charge speed
    ui::drawCard(c, 20, 158, 328, 98, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawSectionLabel(c, 34, 180, "充电速度");
    ui::drawSegmented(c, CHG_SPD_SEG, CHG_SPD_LABELS, 3, (int)cfg.chargeSpeed,
                      ui::COL_MINT, ui::fontCjk14());
    char cap2[64];
    snprintf(cap2, sizeof(cap2), "亮屏 %umA · 熄屏 %umA",
             (unsigned)chargeBaseMa(cfg.chargeSpeed, true),
             (unsigned)chargeBaseMa(cfg.chargeSpeed, false));
    ui::drawText(c, ui::fontCjk14(), 34, 246, cap2, ui::COL_MUTED, ui::COL_CARD);

    // Card 3: live battery status (read-only)
    ui::drawCard(c, 20, 258, 328, 114, ui::COL_CARD, ui::COL_BORDER, 16);
    ui::drawText(c, ui::fontCjk14(), 34, 282, "电池状态", ui::COL_MINT, ui::COL_CARD);
    char b1[48];
    snprintf(b1, sizeof(b1), "%u%% · %.2fV", (unsigned)st.battPct, st.battMv / 1000.0f);
    ui::drawText(c, ui::fontCjk14(), 34, 308, b1, ui::COL_TEXT, ui::COL_CARD);

    char b2[64];
    if (st.gaugePct >= 0) {
      snprintf(b2, sizeof(b2), "温度 %d°C · 库仑计 %d%%",
               (int)st.battTempC, (int)st.gaugePct);
    } else {
      snprintf(b2, sizeof(b2), "温度 %d°C", (int)st.battTempC);
    }
    ui::drawText(c, ui::fontCjk14(), 34, 332, b2, ui::COL_DIM, ui::COL_CARD);
    ui::drawTextRight(c, ui::fontCjk14(), 334, 332,
                      powerStageLabel(st, cfg), ui::COL_ACCENT, ui::COL_CARD);

    char b3[48];
    if (st.chgMa > 0) snprintf(b3, sizeof(b3), "当前充电 %umA", (unsigned)st.chgMa);
    else              snprintf(b3, sizeof(b3), "充电已暂停");
    ui::drawText(c, ui::fontCjk14(), 34, 356, b3, ui::COL_MUTED, ui::COL_CARD);
  }

  if (s_tab == 2) {
    ui::drawButton(c, BTN_PWR_BACK, "完成返回", true, ui::COL_ACCENT, ui::fontCjk16());
  } else {
    ui::drawButton(c, BTN_BACK, "完成返回", true, ui::COL_ACCENT, ui::fontCjk16());
  }
}

UiAction pageSettingsTap(int16_t x, int16_t y, const NovaSettings& cfg) {
  if (HDR_BACK.contains(x, y)) {
    s_reprovConfirm = false;
    return UiAction{ UiAction::GO_PAGE, (int)UiPage::DETAIL };
  }
  if (TAB_BASIC.contains(x, y) && s_tab != 0) {
    s_tab = 0;
    s_reprovConfirm = false;
    return UiAction{ UiAction::SETTINGS_REDRAW, 0 };
  }
  if (TAB_ADV.contains(x, y) && s_tab != 1) {
    s_tab = 1;
    s_reprovConfirm = false;
    return UiAction{ UiAction::SETTINGS_REDRAW, 1 };
  }
  if (TAB_PWR.contains(x, y) && s_tab != 2) {
    s_tab = 2;
    s_reprovConfirm = false;
    return UiAction{ UiAction::SETTINGS_REDRAW, 3 };
  }

  if (s_tab == 0) {
    for (int i = 0; i < 3; i++) {
      if (HOME_BTNS[i].contains(x, y)) return UiAction{ UiAction::SET_HOME, i };
      if (PWR_BTNS[i].contains(x, y))  return UiAction{ UiAction::SET_POWER, i };
    }
    for (int i = 0; i < 5; i++) {
      if (SND_STEPS[i].contains(x, y)) {
        return UiAction{ UiAction::SET_SOUND, (int)SND_VALUES[i] };
      }
      if (BRT_STEPS[i].contains(x, y)) return UiAction{ UiAction::SET_BRIGHT, i };
    }
    if (BTN_TEST_SND.contains(x, y)) return UiAction{ UiAction::TEST_SOUND, 0 };
  } else if (s_tab == 1) {
    if (BTN_TTS_TOGGLE.contains(x, y)) {
      return UiAction{ UiAction::SET_TTS, cfg.ttsEnabled ? 0 : 1 };
    }
    if (BTN_QUIET_TOGGLE.contains(x, y)) {
      return UiAction{ UiAction::SET_QUIET, cfg.quietHours ? 0 : 1 };
    }
    if (BTN_CHOREO_TOGGLE.contains(x, y)) {
      return UiAction{ UiAction::SET_CHOREO, cfg.choreoEnabled ? 0 : 1 };
    }
    for (int i = 0; i < 4; i++) {
      if (SLEEP_BTNS[i].contains(x, y)) {
        return UiAction{ UiAction::SET_BATSLEEP, (int)SLEEP_SECS[i] };
      }
    }
    if (BTN_REPROV.contains(x, y)) {
      uint32_t nowMs = millis();
      if (!s_reprovConfirm || (nowMs - s_reprovAtMs >= REPROV_CONFIRM_TIMEOUT_MS)) {
        s_reprovConfirm = true;
        s_reprovAtMs = nowMs;
        return UiAction{ UiAction::SETTINGS_REDRAW, 2 };
      }
      s_reprovConfirm = false;
      return UiAction{ UiAction::REPROVISION, 0 };
    }
  } else {
    for (int i = 0; i < 3; i++) {
      if (CHG_LIM_BTNS[i].contains(x, y)) {
        return UiAction{ UiAction::SET_CHGLIMIT, (int)CHG_LIM_VALS[i] };
      }
      if (CHG_SPD_BTNS[i].contains(x, y)) {
        return UiAction{ UiAction::SET_CHGSPEED, i };
      }
    }
    if (BTN_PWR_BACK.contains(x, y)) {
      s_reprovConfirm = false;
      return UiAction{ UiAction::GO_PAGE, (int)UiPage::DETAIL };
    }
  }

  if (BTN_BACK.contains(x, y)) {
    s_reprovConfirm = false;
    return UiAction{ UiAction::GO_PAGE, (int)UiPage::DETAIL };
  }
  return UiAction{};
}

bool pageSettingsHitRect(int16_t x, int16_t y, const NovaState&, const NovaSettings&,
                         ui::Rect* out) {
  if (!out) return false;
  if (HDR_BACK.contains(x, y))  { *out = HDR_BACK;  return true; }
  if (TAB_BASIC.contains(x, y)) { *out = TAB_BASIC; return true; }
  if (TAB_ADV.contains(x, y))   { *out = TAB_ADV;   return true; }
  if (TAB_PWR.contains(x, y))   { *out = TAB_PWR;   return true; }
  if (s_tab == 0) {
    for (int i = 0; i < 3; i++) {
      if (HOME_BTNS[i].contains(x, y)) { *out = HOME_BTNS[i]; return true; }
      if (PWR_BTNS[i].contains(x, y))  { *out = PWR_BTNS[i];  return true; }
    }
    for (int i = 0; i < 5; i++) {
      if (SND_STEPS[i].contains(x, y)) { *out = SND_STEPS[i]; return true; }
      if (BRT_STEPS[i].contains(x, y)) { *out = BRT_STEPS[i]; return true; }
    }
    if (BTN_TEST_SND.contains(x, y)) { *out = BTN_TEST_SND; return true; }
  } else if (s_tab == 1) {
    if (BTN_TTS_TOGGLE.contains(x, y))    { *out = BTN_TTS_TOGGLE;    return true; }
    if (BTN_QUIET_TOGGLE.contains(x, y))  { *out = BTN_QUIET_TOGGLE;  return true; }
    if (BTN_CHOREO_TOGGLE.contains(x, y)) { *out = BTN_CHOREO_TOGGLE; return true; }
    for (int i = 0; i < 4; i++) {
      if (SLEEP_BTNS[i].contains(x, y)) { *out = SLEEP_BTNS[i]; return true; }
    }
    if (BTN_REPROV.contains(x, y)) { *out = BTN_REPROV; return true; }
  } else {
    for (int i = 0; i < 3; i++) {
      if (CHG_LIM_BTNS[i].contains(x, y)) { *out = CHG_LIM_BTNS[i]; return true; }
      if (CHG_SPD_BTNS[i].contains(x, y)) { *out = CHG_SPD_BTNS[i]; return true; }
    }
    if (BTN_PWR_BACK.contains(x, y)) { *out = BTN_PWR_BACK; return true; }
  }
  if (BTN_BACK.contains(x, y)) { *out = BTN_BACK; return true; }
  return false;
}
