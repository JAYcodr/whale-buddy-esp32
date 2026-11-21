#include "ui/pages.h"
#include "ui/theme.h"
#include <stdio.h>
#include <string.h>

namespace {
constexpr ui::Rect BTN_DENY { 36, 302, 140, 52 };
constexpr ui::Rect BTN_ALLOW{ 192, 302, 140, 52 };
constexpr ui::Rect BTN_TO_PC{ 36, 366, 296, 44 };

constexpr int16_t kBarX   = 36;
constexpr int16_t kBarY   = 92;
constexpr int16_t kBarW   = 296;
constexpr int16_t kBarH   = 8;
constexpr int16_t kBarRad = kBarH / 2;

struct ApprovalTiming {
  uint32_t rem;
  uint32_t ttl;
  bool     urgent;
  uint16_t frameCol;
};

ApprovalTiming computeTiming(const NovaState& st, uint32_t nowMs) {
  uint32_t elapsed = (nowMs >= st.apArrivedMs) ? (nowMs - st.apArrivedMs) : 0;
  uint32_t ttl = st.apTtlMs ? st.apTtlMs : 15000u;
  uint32_t rem = (elapsed >= ttl) ? 0 : (ttl - elapsed);
  bool urgent = (rem <= 3000u);
  uint16_t frameCol = ui::COL_AMBER;
  if (urgent) {
    // 2Hz breathing (500ms period) between COL_RED and a muted card-blended red.
    uint32_t ph = nowMs % 500u;
    uint8_t t = (ph < 250u) ? (uint8_t)(ph * 255u / 250u)
                            : (uint8_t)((500u - ph) * 255u / 250u);
    frameCol = ui::blend565(ui::COL_RED, ui::blend565(ui::COL_RED, ui::COL_CARD, 128), t);
  }
  return ApprovalTiming{ rem, ttl, urgent, frameCol };
}
}  // namespace

void pageApprovalDrawBar(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  ApprovalTiming t = computeTiming(st, nowMs);
  // Reset bar rect to card background before AA track/fill to avoid corner alpha buildup.
  c.fillRect(kBarX, kBarY, kBarW, kBarH, ui::COL_CARD);
  ui::aaFillRoundRect(c, kBarX, kBarY, kBarW, kBarH, kBarRad, ui::COL_TRACK, ui::COL_TRACK);

  int16_t fw = (int16_t)(((uint64_t)kBarW * t.rem + t.ttl / 2) / t.ttl);
  if (fw > 0) {
    if (fw < kBarH) fw = kBarH;
    if (fw > kBarW) fw = kBarW;
    ui::aaFillRoundRect(c, kBarX, kBarY, fw, kBarH, kBarRad, t.frameCol, t.frameCol);
  }
}

void pageApprovalDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  c.fillScreen(ui::COL_BG);

  ApprovalTiming t = computeTiming(st, nowMs);

  ui::drawCard(c, 20, 32, 328, 394, ui::COL_CARD, t.frameCol, 22);

  // Alert indicator & large numeric countdown
  ui::aaFillCircle(c, 42, 62, 5, t.frameCol);
  ui::drawText(c, ui::fontCjk16(), 56, 69, "请求工具确认", ui::COL_TEXT, ui::COL_CARD);

  char secStr[16];
  snprintf(secStr, sizeof(secStr), "%us", (unsigned)((t.rem + 999) / 1000));
  ui::drawTextRight(c, ui::fontNum24(), 330, 74, secStr, t.frameCol, ui::COL_CARD);

  // Smooth countdown progress bar
  pageApprovalDrawBar(c, st, nowMs);

  // Tool header card
  ui::drawCard(c, 36, 112, 296, 46, ui::COL_CARD_HI, ui::COL_BORDER, 12);
  ui::aaFillRoundRect(c, 46, 122, 48, 26, 7, rgb565(18, 44, 68), rgb565(18, 44, 68));
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), { 46, 122, 48, 26 }, "工具", ui::COL_ACCENT, rgb565(18, 44, 68));

  char toolBuf[48];
  ui::fitUtf8Line(c, ui::fontAscii18(), st.apTool[0] ? st.apTool : "Tool", 216, toolBuf, sizeof(toolBuf));
  ui::drawText(c, ui::fontAscii18(), 104, 143, toolBuf, ui::COL_TEXT, ui::COL_CARD_HI);

  // Multi-line reason box (UTF-8 safe wrap)
  ui::drawCard(c, 36, 168, 296, 122, rgb565(8, 12, 20), ui::COL_BORDER, 12);
  const char* reason = st.apReason[0] ? st.apReason : "DSH 请求执行该操作";
  ui::drawWrappedUtf8(c, ui::fontCjk14(), 48, 195, 272, 24, 4, reason, ui::COL_TEXT, rgb565(8, 12, 20));

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

bool pageApprovalHitRect(int16_t x, int16_t y, const NovaState&, const NovaSettings&, ui::Rect* out) {
  if (!out) return false;
  if (BTN_ALLOW.contains(x, y)) { *out = BTN_ALLOW; return true; }
  if (BTN_DENY.contains(x, y))  { *out = BTN_DENY;  return true; }
  if (BTN_TO_PC.contains(x, y)) { *out = BTN_TO_PC; return true; }
  return false;
}
