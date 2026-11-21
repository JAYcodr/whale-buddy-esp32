#include "ui/pages.h"
#include "ui/theme.h"
#include <stdio.h>

namespace {

constexpr ui::Rect HOME_BTNS[3] = {
  { 34,  92, 94, 36 },
  { 137, 92, 94, 36 },
  { 240, 92, 94, 36 },
};
constexpr const char* HOME_LABELS[3] = { "宠物", "订阅", "榜单" };

constexpr ui::Rect PWR_BTNS[3] = {
  { 34,  176, 94, 36 },
  { 137, 176, 94, 36 },
  { 240, 176, 94, 36 },
};
constexpr const char* PWR_LABELS[3] = { "自适应", "长续航", "灵动" };

constexpr ui::Rect BTN_SOUND{ 206, 244, 128, 34 };

constexpr ui::Rect BRT_BTNS[5] = {
  { 34,  318, 56, 38 },
  { 100, 318, 56, 38 },
  { 166, 318, 56, 38 },
  { 232, 318, 56, 38 },
  { 298, 318, 36, 38 },
};
// Wait: make 5 equal buttons inside 34..334 (300px): 5*54 + 4*7.5 = 270+30 = 300px -> pitch 62px!
constexpr ui::Rect BRT_STEPS[5] = {
  { 34,  318, 52, 38 },
  { 96,  318, 52, 38 },
  { 158, 318, 52, 38 },
  { 220, 318, 52, 38 },
  { 282, 318, 52, 38 },
};

constexpr ui::Rect BTN_REPROV{ 24,  384, 162, 44 };
constexpr ui::Rect BTN_BACK{   198, 384, 146, 44 };

}  // namespace

void pageSettingsDraw(NovaCanvas& c, const NovaSettings& cfg) {
  c.fillScreen(ui::COL_BG);

  ui::aaFillCircle(c, 28, 36, 4, ui::COL_ACCENT);
  ui::drawText(c, ui::fontCjk16(), 40, 43, "系统与偏好设置", ui::COL_TEXT, ui::COL_BG);
  ui::drawTextRight(c, ui::fontAscii14(), 344, 42, "v0.3", ui::COL_MUTED, ui::COL_BG);

  // Card 1: Default home page
  ui::drawCard(c, 20, 58, 328, 82, ui::COL_CARD, ui::COL_BORDER, 16);
  ui::drawText(c, ui::fontCjk14(), 34, 83, "默认首页（开机 / Key1 返回）", ui::COL_DIM, ui::COL_CARD);
  for (int i = 0; i < 3; i++) {
    ui::drawButton(c, HOME_BTNS[i], HOME_LABELS[i], (int)cfg.homePage == i, ui::COL_ACCENT, ui::fontCjk14());
  }

  // Card 2: Power & FPS profile
  ui::drawCard(c, 20, 146, 328, 82, ui::COL_CARD, ui::COL_BORDER, 16);
  ui::drawText(c, ui::fontCjk14(), 34, 171, "功耗与刷新策略", ui::COL_DIM, ui::COL_CARD);
  for (int i = 0; i < 3; i++) {
    ui::drawButton(c, PWR_BTNS[i], PWR_LABELS[i], (int)cfg.powerProfile == i, ui::COL_MINT, ui::fontCjk14());
  }

  // Card 3: Sound & Brightness
  ui::drawCard(c, 20, 234, 328, 140, ui::COL_CARD, ui::COL_BORDER, 16);
  ui::drawText(c, ui::fontCjk14(), 34, 268, "提示音效", ui::COL_TEXT, ui::COL_CARD);
  char sndBuf[20];
  snprintf(sndBuf, sizeof(sndBuf), cfg.soundPct > 0 ? "开启 %u%%" : "静音", cfg.soundPct);
  ui::drawButton(c, BTN_SOUND, sndBuf, cfg.soundPct > 0, ui::COL_ACCENT, ui::fontCjk14());

  c.drawFastHLine(34, 290, 300, rgb565(20, 32, 52));
  ui::drawText(c, ui::fontCjk14(), 34, 311, "屏幕亮度", ui::COL_TEXT, ui::COL_CARD);
  const char* pctLabels[5] = { "20%", "40%", "60%", "80%", "100%" };
  for (int i = 0; i < 5; i++) {
    bool lit = (i <= cfg.brightLevel);
    bool cur = (i == cfg.brightLevel);
    ui::Rect r = BRT_STEPS[i];
    uint16_t bg = cur ? ui::COL_AMBER : (lit ? rgb565(34, 42, 62) : ui::COL_CARD_HI);
    uint16_t fg = cur ? ui::COL_BG : (lit ? ui::COL_AMBER : ui::COL_DIM);
    uint16_t bd = cur ? ui::COL_AMBER : (lit ? rgb565(80, 68, 34) : ui::COL_BORDER);
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 10, bg, bd);
    ui::drawTextCenteredInRect(c, ui::fontAscii14(), r, pctLabels[i], fg, bg);
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
  if (BTN_SOUND.contains(x, y)) return UiAction{ UiAction::TOGGLE_SOUND, 0 };
  for (int i = 0; i < 5; i++) {
    if (BRT_STEPS[i].contains(x, y)) return UiAction{ UiAction::SET_BRIGHT, i };
  }
  if (BTN_REPROV.contains(x, y)) return UiAction{ UiAction::REPROVISION, 0 };
  if (BTN_BACK.contains(x, y))   return UiAction{ UiAction::GO_PAGE, (int)UiPage::DETAIL };
  return UiAction{};
}
