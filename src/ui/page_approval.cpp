#include "ui/pages.h"
#include "ui/theme.h"
#include "ui/tide_palette.h"
#include <stdio.h>
#include <string.h>

namespace {
// ── 原版（OCEAN）矩形 ────────────────────────────────────────────────
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

// ── 主题感知热区：Draw / Tap / HitRect / BarRect 四处共用同一组 helper ──
inline ui::Rect btnDenyRect() {
  return ui::isTideTheme() ? ui::Rect{ 18, 366, 184, 54 }
                           : BTN_DENY;
}
inline ui::Rect btnAllowRect() {
  return ui::isTideTheme() ? ui::Rect{ 212, 366, 138, 54 }
                           : BTN_ALLOW;
}
inline ui::Rect btnToPcRect() {
  return ui::isTideTheme() ? ui::Rect{ 18, 318, 332, 38 }
                           : BTN_TO_PC;
}
inline ui::Rect barRectActive() {
  return ui::isTideTheme() ? ui::Rect{ 18, 12, UI_W - 36, 5 }
                           : ui::Rect{ kBarX, kBarY, kBarW, kBarH };
}

// 潮汐版整页绘制（Carbon 终审打磨版）
void drawApprovalTide(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  using namespace ui::tide;
  c.fillScreen(BG_CRIMSON);
  pageApprovalDrawBar(c, st, nowMs);

  // 顶行提示（避开 AMOLED 硬件圆角）
  ui::drawText(c, ui::fontAscii14(), 18, 36, "STOP // INTERCEPT", WHITE, BG_CRIMSON);
  ui::drawTextRight(c, ui::fontCjk14(), UI_W - 18, 36, "超时自动拒", dimOn(BG_CRIMSON), BG_CRIMSON);

  // 超大倒计时数字（fontNum92 压迫感）+ 右侧工具名与提示
  ApprovalTiming t = computeTiming(st, nowMs);
  char secNum[12];
  snprintf(secNum, sizeof(secNum), "%u", (unsigned)((t.rem + 999) / 1000));
  ui::drawText(c, ui::fontNum92(), 18, 114, secNum, WHITE, BG_CRIMSON);
  {
    ui::TextMetrics sm = ui::measureText(c, ui::fontNum92(), secNum);
    ui::drawText(c, ui::fontAscii18(), (int16_t)(18 + sm.w + 4), 112, "s",
                 dimOn(BG_CRIMSON), BG_CRIMSON);
  }

  char toolLine[40];
  snprintf(toolLine, sizeof(toolLine), "工具 · %s", st.apTool[0] ? st.apTool : "Tool");
  ui::drawTextRight(c, ui::fontCjk16(), UI_W - 18, 80, toolLine, WHITE, BG_CRIMSON);
  ui::drawTextRight(c, ui::fontCjk14(), UI_W - 18, 106, "危险操作待确认",
                    dimOn(BG_CRIMSON), BG_CRIMSON);

  // 中部纯黑高对比命令盒（0px 圆角，左 4px 纯白竖标 + 内部分割线）
  c.fillRect(18, 130, UI_W - 36, 176, SURF_DARK);
  c.fillRect(18, 130, 4, 176, WHITE);
  ui::drawText(c, ui::fontAscii14(), 34, 155, "TOOL // COMMAND", rgb565(255, 118, 132), SURF_DARK);
  c.fillRect(34, 165, UI_W - 68, 1, rgb565(56, 26, 32));
  const char* reason = st.apReason[0] ? st.apReason : "DSH 请求执行该操作";
  ui::drawWrappedUtf8(c, ui::fontCjk16(), 34, 194, UI_W - 68, 28, 4, reason, WHITE, SURF_DARK);

  // 次要操作：转电脑端处理（细白框）
  const ui::Rect rPc = btnToPcRect();
  c.drawRect(rPc.x, rPc.y, rPc.w, rPc.h, CRIM_LINE);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), rPc, "转电脑端处理", WHITE, BG_CRIMSON);

  // 主操作：左纯白实心「拒绝拦截」（面积更大最防误触），右 2px 白描边「批准执行」
  const ui::Rect rDeny = btnDenyRect();
  c.fillRect(rDeny.x, rDeny.y, rDeny.w, rDeny.h, WHITE);
  ui::drawTextCenteredInRect(c, ui::fontCjk16(), rDeny, "拒绝拦截", BG_CRIMSON, WHITE);

  const ui::Rect rAllow = btnAllowRect();
  c.drawRect(rAllow.x, rAllow.y, rAllow.w, rAllow.h, WHITE);
  c.drawRect(rAllow.x + 1, rAllow.y + 1, rAllow.w - 2, rAllow.h - 2, WHITE);
  ui::drawTextCenteredInRect(c, ui::fontCjk16(), rAllow, "批准执行", WHITE, BG_CRIMSON);
}

}  // namespace

void pageApprovalDrawBar(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  ApprovalTiming t = computeTiming(st, nowMs);
  if (ui::isTideTheme()) {
    const ui::Rect br = barRectActive();
    const uint16_t track = ui::blend565(ui::tide::BG_CRIMSON, ui::tide::WHITE, 64);
    c.fillRect(br.x, br.y, br.w, br.h, track);
    int16_t fw = (int16_t)(((uint64_t)br.w * t.rem + t.ttl / 2) / t.ttl);
    if (fw > 0) c.fillRect(br.x, br.y, fw, br.h, ui::tide::WHITE);
    return;
  }

  // 原版 OCEAN 代码保持一字不改。
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
  if (ui::isTideTheme()) {
    drawApprovalTide(c, st, nowMs);
    return;
  }

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
  const ui::Rect rAllow = btnAllowRect();
  const ui::Rect rDeny  = btnDenyRect();
  const ui::Rect rPc    = btnToPcRect();
  if (rAllow.contains(x, y)) return UiAction{ UiAction::APPROVE, 0 };
  if (rDeny.contains(x, y))  return UiAction{ UiAction::DENY, 0 };
  if (rPc.contains(x, y))    return UiAction{ UiAction::TO_PC, 0 };
  return UiAction{};
}

bool pageApprovalHitRect(int16_t x, int16_t y, const NovaState&, const NovaSettings&, ui::Rect* out) {
  if (!out) return false;
  const ui::Rect rAllow = btnAllowRect();
  const ui::Rect rDeny  = btnDenyRect();
  const ui::Rect rPc    = btnToPcRect();
  if (rAllow.contains(x, y)) { *out = rAllow; return true; }
  if (rDeny.contains(x, y))  { *out = rDeny;  return true; }
  if (rPc.contains(x, y))    { *out = rPc;    return true; }
  return false;
}

ui::Rect pageApprovalBarRect() {
  return barRectActive();
}
