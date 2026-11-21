#include "ui/pages.h"
#include "anim/nva.h"
#include "ui/theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr ui::Rect BTN_CANCEL_PORTAL{ 94, 304, 180, 44 };
constexpr int ANIM_RX = 94, ANIM_RY = 86, ANIM_RW = 180, ANIM_RH = 176;
constexpr int PET_CX  = ANIM_RX + ANIM_RW / 2;
constexpr int PET_CY  = ANIM_RY + ANIM_RH / 2;

int s_sparkFrames = 0;

void drawSpark(NovaCanvas& c, int16_t x, int16_t y, uint16_t col, int16_t size) {
  c.drawLine(x - size, y, x + size, y, col);
  c.drawLine(x, y - size, x, y + size, col);
}

void drawStageAnimContent(NovaCanvas& c, const NovaState& st, uint32_t frame) {
  c.fillRect(ANIM_RX, ANIM_RY, ANIM_RW, ANIM_RH, ui::COL_BG);

  const float t = frame * 0.075f;
  const bool busy = (st.run > 0 || st.wait > 0);
  const uint16_t auraCol = st.wait > 0 ? ui::COL_AMBER : (busy ? ui::COL_MINT : ui::COL_ACCENT);

  // Subtle concentric AMOLED halo rings (anti-aliased)
  ui::aaDrawCircle(c, PET_CX, PET_CY + 2, 78, rgb565(10, 24, 44));
  ui::aaDrawCircle(c, PET_CX, PET_CY + 2, 60, busy ? rgb565(14, 38, 46) : rgb565(14, 32, 54));

  // Gentle bioluminescent ambient particles
  for (int i = 0; i < 4; i++) {
    float a = t * (busy ? 1.35f : 0.7f) + i * 1.5708f;
    int px = PET_CX + (int)(cosf(a) * 72.0f);
    int py = PET_CY + (int)(sinf(a * 1.15f) * 44.0f);
    ui::aaFillCircle(c, (int16_t)px, (int16_t)py, (i & 1) ? 2 : 3, (i & 1) ? ui::COL_MINT : ui::COL_ACCENT);
  }

  // Interactive tap burst feedback
  if (s_sparkFrames > 0) {
    float prog = 1.0f - (float)s_sparkFrames / 24.0f;
    int ringR = 32 + (int)(prog * 52.0f);
    uint16_t ringCol = (s_sparkFrames > 10) ? ui::COL_ACCENT : rgb565(24, 72, 110);
    ui::aaDrawCircle(c, PET_CX, PET_CY, (int16_t)ringR, ringCol);
    for (int i = 0; i < 6; i++) {
      float a = prog * 0.6f + i * 1.0472f;
      int d = 40 + (int)(prog * 44.0f);
      int sx = PET_CX + (int)(cosf(a) * d);
      int sy = PET_CY + (int)(sinf(a * 1.0f) * d);
      if (sx > ANIM_RX + 4 && sx < ANIM_RX + ANIM_RW - 4 &&
          sy > ANIM_RY + 4 && sy < ANIM_RY + ANIM_RH - 4) {
        drawSpark(c, sx, sy, (i & 1) ? ui::COL_MINT : ui::COL_GOLD, (s_sparkFrames > 12) ? 3 : 2);
      }
    }
    s_sparkFrames--;
  }

  if (!nvaDraw(c, PET_CX, ANIM_RY + ANIM_RH - 6)) {
    ui::aaFillCircle(c, PET_CX, PET_CY, 34, auraCol);
    ui::aaFillCircle(c, PET_CX - 10, PET_CY - 10, 9, rgb565(224, 246, 255));
  }
}

void drawStatusPill(NovaCanvas& c, const NovaState& st) {
  uint16_t dotCol = ui::COL_ACCENT;
  const char* stateTag = "待机陪伴中";
  if (st.wait > 0) {
    dotCol = ui::COL_AMBER;
    stateTag = "等待确认";
  } else if (st.run > 0) {
    dotCol = ui::COL_MINT;
    stateTag = "思考执行中";
  }
  ui::TextMetrics tm = ui::measureText(c, ui::fontCjk14(), stateTag);
  int16_t pw = (int16_t)tm.w + 30;
  int16_t px = (UI_W - pw) / 2;
  int16_t py = 272;
  ui::drawCard(c, px, py, pw, 26, ui::COL_CARD, ui::COL_BORDER, 13);
  ui::aaFillCircle(c, px + 12, py + 13, 3, dotCol);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), { (int16_t)(px + 18), py, (int16_t)(pw - 22), 26 },
                             stateTag, ui::COL_DIM, ui::COL_CARD);
}
}  // namespace

void pageHomeDrawStage(NovaCanvas& c, const NovaState& st, uint32_t frame,
                       int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH) {
  drawStageAnimContent(c, st, frame);
  if (outX) *outX = ANIM_RX;
  if (outY) *outY = ANIM_RY;
  if (outW) *outW = ANIM_RW;
  if (outH) *outH = ANIM_RH;
}

void pageHomeDraw(NovaCanvas& c, const NovaState& st, uint32_t frame) {
  c.fillScreen(ui::COL_BG);
  ui::drawTopBar(c, st, 0);

  if (st.wifiApMode && !st.wifiConnected) {
    ui::drawCard(c, 24, 80, 320, 290, ui::COL_CARD, ui::COL_AMBER, 20);
    ui::drawText(c, ui::fontCjk16(), 44, 118, "首次配网 / AP 模式", ui::COL_AMBER, ui::COL_CARD);

    char step1[64];
    snprintf(step1, sizeof(step1), "1. 手机连接热点: %s", st.apSsid[0] ? st.apSsid : "Nova-XXXX");
    ui::drawText(c, ui::fontCjk14(), 44, 158, step1, ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), 44, 188, "2. 浏览器访问: http://192.168.4.1", ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), 44, 218, "3. 填写 WiFi 密码与 MQTT Broker", ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), 44, 256, "默认 Broker: 192.168.31.15:1883", ui::COL_DIM, ui::COL_CARD);

    ui::drawButton(c, BTN_CANCEL_PORTAL, "暂不配网", false, ui::COL_AMBER);
    return;
  }

  drawStageAnimContent(c, st, frame);
  drawStatusPill(c, st);

  // Bottom DSH Live Dock Card
  const int16_t dockX = 24, dockY = 312, dockW = 320, dockH = 114;
  ui::drawCard(c, dockX, dockY, dockW, dockH, ui::COL_CARD, ui::COL_BORDER, 18);

  char statsLine[64];
  snprintf(statsLine, sizeof(statsLine), "运行 %u · 等待 %u · 会话 %u", st.run, st.wait, st.sess);
  ui::drawText(c, ui::fontCjk14(), dockX + 16, dockY + 28, statsLine, ui::COL_TEXT, ui::COL_CARD);

  if (st.ctxPct >= 0) {
    char ctxBuf[16];
    snprintf(ctxBuf, sizeof(ctxBuf), "CTX %d%%", st.ctxPct);
    uint16_t ctxCol = st.ctxPct >= 80 ? ui::COL_RED : (st.ctxPct >= 60 ? ui::COL_AMBER : ui::COL_MINT);
    ui::TextMetrics cm = ui::measureText(c, ui::fontAscii14(), ctxBuf);
    int16_t pw = (int16_t)cm.w + 16;
    int16_t px = (int16_t)(dockX + dockW - 14 - pw);
    int16_t py = (int16_t)(dockY + 12);
    ui::aaFillRoundRect(c, px, py, pw, 22, 11, rgb565(18, 30, 48), rgb565(18, 30, 48));
    ui::drawTextCenteredInRect(c, ui::fontAscii14(), { px, py, pw, 22 }, ctxBuf, ctxCol, rgb565(18, 30, 48));
  }

  char cmdLine[96];
  ui::fitUtf8Line(c, ui::fontCjk16(), st.msg[0] ? st.msg : "等待 DSH 事件...", dockW - 32, cmdLine, sizeof(cmdLine));
  ui::drawText(c, ui::fontCjk16(), dockX + 16, dockY + 64, cmdLine, ui::COL_ACCENT, ui::COL_CARD);

  char metaLine[96];
  if (st.activeModel[0]) {
    char raw[80];
    snprintf(raw, sizeof(raw), "当前模型 · %s", st.activeModel);
    ui::fitUtf8Line(c, ui::fontCjk14(), raw, dockW - 32, metaLine, sizeof(metaLine));
  } else {
    snprintf(metaLine, sizeof(metaLine), "轻触诺瓦互动 · 左右滑动切页");
  }
  ui::drawText(c, ui::fontCjk14(), dockX + 16, dockY + 94, metaLine, ui::COL_DIM, ui::COL_CARD);
}

UiAction pageHomeTap(int16_t x, int16_t y, const NovaState& st) {
  if (st.wifiApMode && !st.wifiConnected && BTN_CANCEL_PORTAL.contains(x, y)) {
    return UiAction{ UiAction::CANCEL_PORTAL, 0 };
  }
  if (x >= ANIM_RX - 20 && x <= ANIM_RX + ANIM_RW + 20 &&
      y >= ANIM_RY - 10 && y <= ANIM_RY + ANIM_RH + 20) {
    s_sparkFrames = 24;
    return UiAction{ UiAction::PET_TAP, 0 };
  }
  return UiAction{};
}
