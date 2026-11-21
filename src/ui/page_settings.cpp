#include "ui/pages.h"
#include "ui/theme.h"
#include <stdio.h>

namespace {

constexpr ui::Rect HOME_BTNS[3] = {
  { 34,  84, 94, 34 },
  { 137, 84, 94, 34 },
  { 240, 84, 94, 34 },
};
constexpr const char* HOME_LABELS[3] = { "宠物", "订阅", "榜单" };

constexpr ui::Rect PWR_BTNS[3] = {
  { 34,  164, 94, 34 },
  { 137, 164, 94, 34 },
  { 240, 164, 94, 34 },
};
constexpr const char* PWR_LABELS[3] = { "自适应", "长续航", "灵动" };

constexpr ui::Rect SND_STEPS[5] = {
  { 34,  246, 52, 36 },
  { 96,  246, 52, 36 },
  { 158, 246, 52, 36 },
  { 220, 246, 52, 36 },
  { 282, 246, 52, 36 },
};
constexpr uint8_t SND_VALUES[5] = { 0, 25, 50, 75, 100 };
constexpr const char* SND_LABELS[5] = { "静音", "25%", "50%", "75%", "100%" };

constexpr ui::Rect BRT_STEPS[5] = {
  { 34,  326, 52, 38 },
  { 96,  326, 52, 38 },
  { 158, 326, 52, 38 },
  { 220, 326, 52, 38 },
  { 282, 326, 52, 38 },
};
constexpr const char* BRT_LABELS[5] = { "20%", "40%", "60%", "80%", "100%" };

constexpr ui::Rect BTN_REPROV{ 24,  386, 162, 44 };
constexpr ui::Rect BTN_BACK{   198, 386, 146, 44 };

int soundStepIndex(uint8_t pct) {
  if (pct == 0) return 0;
  if (pct <= 35) return 1;
  if (pct <= 60) return 2;
  if (pct <= 85) return 3;
  return 4;
}

}  // namespace

void pageSettingsDraw(NovaCanvas& c, const NovaSettings& cfg) {
  c.fillScreen(ui::COL_BG);

  ui::aaFillCircle(c, 28, 32, 4, ui::COL_ACCENT);
  ui::drawText(c, ui::fontCjk16(), 40, 39, "系统与偏好设置", ui::COL_TEXT, ui::COL_BG);
  ui::drawTextRight(c, ui::fontAscii14(), 344, 38, "v0.3", ui::COL_MUTED, ui::COL_BG);

  // Card 1: Default home page
  ui::drawCard(c, 20, 52, 328, 74, ui::COL_CARD, ui::COL_BORDER, 16);
  ui::drawText(c, ui::fontCjk14(), 34, 74, "默认首页（开机 / Key1 返回）", ui::COL_DIM, ui::COL_CARD);
  for (int i = 0; i < 3; i++) {
    ui::drawButton(c, HOME_BTNS[i], HOME_LABELS[i], (int)cfg.homePage == i, ui::COL_ACCENT, ui::fontCjk14());
  }

  // Card 2: Power & FPS profile
  ui::drawCard(c, 20, 132, 328, 74, ui::COL_CARD, ui::COL_BORDER, 16);
  ui::drawText(c, ui::fontCjk14(), 34, 154, "功耗与刷新策略", ui::COL_DIM, ui::COL_CARD);
  for (int i = 0; i < 3; i++) {
    ui::drawButton(c, PWR_BTNS[i], PWR_LABELS[i], (int)cfg.powerProfile == i, ui::COL_MINT, ui::fontCjk14());
  }

  // Card 3: Sound volume & screen brightness (unified 5-step controls)
  ui::drawCard(c, 20, 212, 328, 166, ui::COL_CARD, ui::COL_BORDER, 16);
  ui::drawText(c, ui::fontCjk14(), 34, 236, "语音与提示音量", ui::COL_TEXT, ui::COL_CARD);
  char sndTag[16];
  if (cfg.soundPct == 0) snprintf(sndTag, sizeof(sndTag), "已静音");
  else snprintf(sndTag, sizeof(sndTag), "%u%%", cfg.soundPct);
  ui::drawTextRight(c, ui::fontCjk14(), 334, 236, sndTag,
                    cfg.soundPct == 0 ? ui::COL_RED : ui::COL_ACCENT, ui::COL_CARD);

  int sndIdx = soundStepIndex(cfg.soundPct);
  for (int i = 0; i < 5; i++) {
    bool cur = (i == sndIdx);
    bool lit = (sndIdx > 0 && i > 0 && i <= sndIdx);
    ui::Rect r = SND_STEPS[i];
    uint16_t bg, fg, bd;
    if (i == 0 && cur) {
      bg = ui::COL_RED;
      fg = ui::COL_BG;
      bd = ui::COL_RED;
    } else if (cur) {
      bg = ui::COL_ACCENT;
      fg = ui::COL_BG;
      bd = ui::COL_ACCENT;
    } else if (lit) {
      bg = rgb565(18, 42, 62);
      fg = ui::COL_ACCENT;
      bd = rgb565(38, 86, 120);
    } else {
      bg = ui::COL_CARD_HI;
      fg = ui::COL_DIM;
      bd = ui::COL_BORDER;
    }
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 10, bg, bd);
    ui::drawTextCenteredInRect(c, i == 0 ? ui::fontCjk14() : ui::fontAscii14(), r, SND_LABELS[i], fg, bg);
  }

  c.drawFastHLine(34, 292, 300, rgb565(20, 32, 52));
  ui::drawText(c, ui::fontCjk14(), 34, 314, "屏幕亮度", ui::COL_TEXT, ui::COL_CARD);
  uint8_t brtIdx = (cfg.brightLevel > 4) ? 4 : cfg.brightLevel;
  ui::drawTextRight(c, ui::fontAscii14(), 334, 314, BRT_LABELS[brtIdx], ui::COL_AMBER, ui::COL_CARD);

  for (int i = 0; i < 5; i++) {
    bool lit = (i <= cfg.brightLevel);
    bool cur = (i == cfg.brightLevel);
    ui::Rect r = BRT_STEPS[i];
    uint16_t bg = cur ? ui::COL_AMBER : (lit ? rgb565(34, 42, 62) : ui::COL_CARD_HI);
    uint16_t fg = cur ? ui::COL_BG : (lit ? ui::COL_AMBER : ui::COL_DIM);
    uint16_t bd = cur ? ui::COL_AMBER : (lit ? rgb565(80, 68, 34) : ui::COL_BORDER);
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 10, bg, bd);
    ui::drawTextCenteredInRect(c, ui::fontAscii14(), r, BRT_LABELS[i], fg, bg);
  }

  const uint16_t reprovBg = rgb565(40, 28, 12);
  ui::aaFillRoundRect(c, BTN_REPROV.x, BTN_REPROV.y, BTN_REPROV.w, BTN_REPROV.h, 12, reprovBg, ui::COL_AMBER);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), BTN_REPROV, "重新配网 (AP)", ui::COL_AMBER, reprovBg);

  ui::drawButton(c, BTN_BACK, "完成返回", true, ui::COL_ACCENT, ui::fontCjk16());
}

UiAction pageSettingsTap(int16_t x, int16_t y, const NovaSettings&) {
  for (int i = 0; i < 3; i++) {
    if (HOME_BTNS[i].contains(x, y)) return UiAction{ UiAction::SET_HOME, i };
    if (PWR_BTNS[i].contains(x, y))  return UiAction{ UiAction::SET_POWER, i };
  }
  for (int i = 0; i < 5; i++) {
    if (SND_STEPS[i].contains(x, y)) return UiAction{ UiAction::SET_SOUND, (int)SND_VALUES[i] };
    if (BRT_STEPS[i].contains(x, y)) return UiAction{ UiAction::SET_BRIGHT, i };
  }
  if (BTN_REPROV.contains(x, y)) return UiAction{ UiAction::REPROVISION, 0 };
  if (BTN_BACK.contains(x, y))   return UiAction{ UiAction::GO_PAGE, (int)UiPage::DETAIL };
  return UiAction{};
}
