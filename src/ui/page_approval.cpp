#include "ui/pages.h"
#include "ui/theme.h"
#include <stdio.h>
#include <string.h>

namespace {
constexpr ui::Rect BTN_DENY { 36, 302, 140, 52 };
constexpr ui::Rect BTN_ALLOW{ 192, 302, 140, 52 };
constexpr ui::Rect BTN_TO_PC{ 36, 366, 296, 44 };
}  // namespace

void pageApprovalDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  c.fillScreen(ui::COL_BG);

  uint32_t elapsed = (nowMs >= st.apArrivedMs) ? (nowMs - st.apArrivedMs) : 0;
  uint32_t ttl = st.apTtlMs ? st.apTtlMs : 15000;
  uint32_t rem = (elapsed >= ttl) ? 0 : (ttl - elapsed);
  uint8_t pct = (uint8_t)((uint64_t)rem * 100 / ttl);
  bool urgent = (rem <= 3000);
  uint16_t frameCol = urgent ? ui::COL_RED : ui::COL_AMBER;

  ui::drawCard(c, 20, 32, 328, 394, ui::COL_CARD, frameCol, 22);

  // Alert indicator & large numeric countdown
  ui::aaFillCircle(c, 44, 62, 5, frameCol);
  ui::drawText(c, ui::fontCjk16(), 58, 68, "请求工具执行确认", ui::COL_TEXT, ui::COL_CARD);

  char secStr[16];
  snprintf(secStr, sizeof(secStr), "%us", (unsigned)((rem + 999) / 1000));
  ui::drawTextRight(c, ui::fontNum24(), 330, 72, secStr, frameCol, ui::COL_CARD);

  // Smooth countdown progress bar
  ui::drawProgressBar(c, 36, 90, 296, 8, pct, frameCol);

  // Tool header card
  ui::drawCard(c, 36, 112, 296, 44, ui::COL_CARD_HI, ui::COL_BORDER, 12);
  ui::aaFillRoundRect(c, 46, 122, 46, 24, 6, rgb565(18, 44, 68), rgb565(18, 44, 68));
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), { 46, 122, 46, 24 }, "工具", ui::COL_ACCENT, rgb565(18, 44, 68));

  char toolBuf[48];
  ui::fitUtf8Line(c, ui::fontAscii18(), st.apTool[0] ? st.apTool : "Tool", 220, toolBuf, sizeof(toolBuf));
  ui::drawText(c, ui::fontAscii18(), 102, 141, toolBuf, ui::COL_TEXT, ui::COL_CARD_HI);

  // Multi-line reason box (UTF-8 safe wrap)
  ui::drawCard(c, 36, 168, 296, 120, rgb565(8, 12, 20), ui::COL_BORDER, 12);
  const char* reason = st.apReason[0] ? st.apReason : "DSH 请求执行该操作";
  ui::drawWrappedUtf8(c, ui::fontCjk14(), 48, 194, 272, 24, 4, reason, ui::COL_TEXT, rgb565(8, 12, 20));

  // Deny / Allow primary buttons
  const uint16_t denyBg = rgb565(46, 14, 22);
  ui::aaFillRoundRect(c, BTN_DENY.x, BTN_DENY.y, BTN_DENY.w, BTN_DENY.h, 14, denyBg, ui::COL_RED);
  ui::drawTextCenteredInRect(c, ui::fontCjk16(), BTN_DENY, "拒绝", ui::COL_RED, denyBg);

  ui::drawButton(c, BTN_ALLOW, "批准执行", true, ui::COL_MINT, ui::fontCjk16());

  // Secondary action: handle on desktop
  ui::drawButton(c, BTN_TO_PC, "转电脑端处理", false, ui::COL_DIM, ui::fontCjk14());
}

UiAction pageApprovalTap(int16_t x, int16_t y) {
  if (BTN_ALLOW.contains(x, y)) return UiAction{ UiAction::APPROVE, 0 };
  if (BTN_DENY.contains(x, y))  return UiAction{ UiAction::DENY, 0 };
  if (BTN_TO_PC.contains(x, y)) return UiAction{ UiAction::TO_PC, 0 };
  return UiAction{};
}
