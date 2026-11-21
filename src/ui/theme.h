#pragma once
#include "model/state.h"
#include "ui/aafont.h"
#include "ui/canvas.h"
#include <stdio.h>
#include <string.h>

namespace ui {

// 4-bpp anti-aliased fonts (PingFang SC + DIN Alternate Bold).
const AaFont* fontCjk14();
const AaFont* fontCjk16();
const AaFont* fontAscii14();
const AaFont* fontAscii18();
const AaFont* fontNum24();

// Pure OLED black & deep oceanic glass palette.
constexpr uint16_t COL_BG       = rgb565(0, 0, 0);
constexpr uint16_t COL_CARD     = rgb565(12, 18, 30);
constexpr uint16_t COL_CARD_HI  = rgb565(22, 34, 56);
constexpr uint16_t COL_BORDER   = rgb565(30, 48, 78);
constexpr uint16_t COL_TRACK    = rgb565(18, 28, 46);
constexpr uint16_t COL_TEXT     = rgb565(242, 246, 255);
constexpr uint16_t COL_DIM      = rgb565(130, 148, 180);
constexpr uint16_t COL_MUTED    = rgb565(74, 92, 124);
constexpr uint16_t COL_ACCENT   = rgb565(72, 198, 255);
constexpr uint16_t COL_MINT     = rgb565(64, 230, 176);
constexpr uint16_t COL_AMBER    = rgb565(255, 196, 76);
constexpr uint16_t COL_RED      = rgb565(255, 88, 108);
constexpr uint16_t COL_VIOLET   = rgb565(168, 140, 255);
constexpr uint16_t COL_GOLD     = rgb565(255, 206, 72);
constexpr uint16_t COL_SILVER   = rgb565(198, 212, 232);
constexpr uint16_t COL_BRONZE   = rgb565(224, 146, 86);

struct Rect {
  int16_t x, y, w, h;
  bool contains(int16_t px, int16_t py) const {
    return px >= x && py >= y && px < (x + w) && py < (y + h);
  }
};

struct TextMetrics {
  int16_t  x1, y1;
  uint16_t w, h;
};

TextMetrics measureText(NovaCanvas& c, const AaFont* font, const char* s);
void drawText(NovaCanvas& c, const AaFont* font, int16_t x, int16_t baselineY,
              const char* s, uint16_t fg, uint16_t bg = COL_BG);

void aaFillCircle(NovaCanvas& c, int16_t cx, int16_t cy, int16_t r, uint16_t col);
void aaDrawCircle(NovaCanvas& c, int16_t cx, int16_t cy, int16_t r, uint16_t col);
void aaFillRoundRect(NovaCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                     int16_t r, uint16_t fill, uint16_t border);
void aaDrawRoundRect(NovaCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                     int16_t r, uint16_t border);

inline void drawTextCentered(NovaCanvas& c, const AaFont* font, int16_t centerX, int16_t baselineY,
                             const char* s, uint16_t fg, uint16_t bg = COL_BG) {
  if (!s || !s[0]) return;
  TextMetrics m = measureText(c, font, s);
  int16_t tx = centerX - (int16_t)m.w / 2 - m.x1;
  drawText(c, font, tx, baselineY, s, fg, bg);
}

inline void drawTextRight(NovaCanvas& c, const AaFont* font, int16_t rightX, int16_t baselineY,
                          const char* s, uint16_t fg, uint16_t bg = COL_BG) {
  if (!s || !s[0]) return;
  TextMetrics m = measureText(c, font, s);
  int16_t tx = rightX - (int16_t)m.w - m.x1;
  drawText(c, font, tx, baselineY, s, fg, bg);
}

inline void drawTextCenteredInRect(NovaCanvas& c, const AaFont* font, Rect r,
                                   const char* s, uint16_t fg, uint16_t bg = COL_BG) {
  if (!s || !s[0]) return;
  TextMetrics m = measureText(c, font, s);
  int16_t tx = r.x + (r.w - (int16_t)m.w) / 2 - m.x1;
  int16_t ty = r.y + (r.h - (int16_t)m.h) / 2 - m.y1;
  drawText(c, font, tx, ty, s, fg, bg);
}

inline void drawCard(NovaCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                     uint16_t fill = COL_CARD, uint16_t border = COL_BORDER, int16_t r = 16) {
  aaFillRoundRect(c, x, y, w, h, r, fill, border);
}

inline uint16_t blend565(uint16_t a, uint16_t b, uint8_t t) {
  if (t == 0) return a;
  if (t == 255) return b;
  uint32_t inv = 255 - t;
  uint32_t r  = (((a >> 11) & 0x1F) * inv + ((b >> 11) & 0x1F) * t + 127) / 255;
  uint32_t g  = (((a >> 5)  & 0x3F) * inv + ((b >> 5)  & 0x3F) * t + 127) / 255;
  uint32_t bl = ((a         & 0x1F) * inv + (b         & 0x1F) * t + 127) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

void drawSwitch(NovaCanvas& c, Rect r, bool on, uint16_t accent);
void drawSegmented(NovaCanvas& c, Rect r, const char* const* labels, int n, int sel,
                   uint16_t accent, const AaFont* f = nullptr);
void drawLevelBar(NovaCanvas& c, Rect r, int levels, int cur, uint16_t accent,
                  bool firstIsMute);
void drawSectionLabel(NovaCanvas& c, int16_t x, int16_t baseY, const char* s);
void drawPressedOverlay(NovaCanvas& c, Rect r, int16_t radius);

inline void drawButton(NovaCanvas& c, Rect r, const char* label, bool active,
                       uint16_t accent = COL_ACCENT, const AaFont* font = nullptr) {
  const AaFont* f = font ? font : fontCjk16();
  uint16_t bg = active ? accent : COL_CARD_HI;
  uint16_t fg = active ? COL_BG : COL_TEXT;
  uint16_t bd = active ? accent : COL_BORDER;
  aaFillRoundRect(c, r.x, r.y, r.w, r.h, 12, bg, bd);
  drawTextCenteredInRect(c, f, r, label, fg, bg);
}

inline void drawProgressBar(NovaCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                            uint8_t pct, uint16_t fillCol, uint16_t trackCol = COL_TRACK) {
  if (pct > 100) pct = 100;
  int16_t rad = h / 2;
  aaFillRoundRect(c, x, y, w, h, rad, trackCol, trackCol);
  int16_t fw = (int16_t)((int32_t)w * pct / 100);
  if (fw > 0) {
    if (fw < h) fw = h;
    if (fw > w) fw = w;
    aaFillRoundRect(c, x, y, fw, h, rad, fillCol, fillCol);
  }
}

inline size_t nextUtf8Boundary(const char* s, size_t idx) {
  uint8_t b = (uint8_t)s[idx];
  if (b == 0) return idx;
  size_t len = 1;
  if ((b & 0xE0) == 0xC0) len = 2;
  else if ((b & 0xF0) == 0xE0) len = 3;
  else if ((b & 0xF8) == 0xF0) len = 4;
  for (size_t k = 1; k < len; k++) {
    if ((uint8_t)s[idx + k] == 0) return idx + k;
  }
  return idx + len;
}

inline void fitUtf8Line(NovaCanvas& c, const AaFont* font, const char* src,
                        int16_t maxW, char* dst, size_t dstCap) {
  if (!dst || dstCap == 0) return;
  if (!src || !src[0]) { dst[0] = '\0'; return; }
  char srcCopy[192];
  if (src == dst) {
    strncpy(srcCopy, src, sizeof(srcCopy) - 1);
    srcCopy[sizeof(srcCopy) - 1] = '\0';
    src = srcCopy;
  }
  dst[0] = '\0';

  size_t bestEnd = 0;
  size_t i = 0;
  char tmp[192];
  while (src[i]) {
    size_t nx = nextUtf8Boundary(src, i);
    if (nx >= sizeof(tmp) - 1 || nx >= dstCap - 1) break;
    memcpy(tmp, src, nx);
    tmp[nx] = '\0';
    if ((int16_t)measureText(c, font, tmp).w <= maxW) {
      bestEnd = nx;
      i = nx;
    } else {
      break;
    }
  }
  if (src[bestEnd] == '\0') {
    memcpy(dst, src, bestEnd);
    dst[bestEnd] = '\0';
    return;
  }
  size_t trim = bestEnd;
  while (trim > 0) {
    size_t prev = trim - 1;
    while (prev > 0 && ((uint8_t)src[prev] & 0xC0) == 0x80) prev--;
    if (prev + 3 < sizeof(tmp) && prev + 3 < dstCap) {
      memcpy(tmp, src, prev);
      memcpy(tmp + prev, "...", 4);
      if ((int16_t)measureText(c, font, tmp).w <= maxW) {
        memcpy(dst, tmp, prev + 4);
        return;
      }
    }
    trim = prev;
  }
  snprintf(dst, dstCap, "...");
}

inline int drawWrappedUtf8(NovaCanvas& c, const AaFont* font, int16_t x, int16_t baselineY,
                           int16_t maxW, int16_t lineH, int maxLines,
                           const char* text, uint16_t fg, uint16_t bg = COL_BG) {
  if (!text || !text[0] || maxLines <= 0) return 0;
  const char* p = text;
  int drawn = 0;
  char line[192];

  while (*p && drawn < maxLines) {
    while (*p == ' ') p++;
    if (!*p) break;

    if (drawn == maxLines - 1) {
      fitUtf8Line(c, font, p, maxW, line, sizeof(line));
      drawText(c, font, x, baselineY + drawn * lineH, line, fg, bg);
      drawn++;
      break;
    }

    size_t bestEnd = 0;
    size_t lastSpace = 0;
    size_t i = 0;
    while (p[i] && p[i] != '\n') {
      size_t nx = nextUtf8Boundary(p, i);
      if (nx >= sizeof(line) - 1) break;
      memcpy(line, p, nx);
      line[nx] = '\0';
      if ((int16_t)measureText(c, font, line).w <= maxW) {
        bestEnd = nx;
        if (p[i] == ' ' || p[i] == '/' || p[i] == '-' || p[i] == ':') lastSpace = nx;
        i = nx;
      } else {
        break;
      }
    }
    if (bestEnd == 0) bestEnd = nextUtf8Boundary(p, 0);
    else if (p[bestEnd] && p[bestEnd] != '\n' && lastSpace > 2) bestEnd = lastSpace;

    memcpy(line, p, bestEnd);
    line[bestEnd] = '\0';
    drawText(c, font, x, baselineY + drawn * lineH, line, fg, bg);
    drawn++;
    p += bestEnd;
    if (*p == '\n') p++;
  }
  return drawn;
}

inline void drawBatteryIcon(NovaCanvas& c, int16_t rightX, int16_t centerY,
                            uint8_t pct, bool usbPresent) {
  if (pct > 100) pct = 100;
  char pctStr[10];
  snprintf(pctStr, sizeof(pctStr), "%u%%", pct);

  uint16_t col = usbPresent ? COL_MINT : (pct <= 20 ? COL_RED : (pct <= 45 ? COL_AMBER : COL_TEXT));
  const int16_t bw = 26;
  const int16_t bh = 14;
  const int16_t bx = rightX - bw - 3;
  const int16_t by = centerY - bh / 2;

  aaDrawRoundRect(c, bx, by, bw, bh, 3, COL_DIM);
  c.fillRect(bx + bw, centerY - 3, 2, 6, COL_DIM);
  int16_t fillW = (int16_t)(((bw - 4) * pct + 50) / 100);
  if (fillW < 2 && pct > 0) fillW = 2;
  if (fillW > bw - 4) fillW = bw - 4;
  if (fillW > 0) {
    c.fillRect(bx + 2, by + 2, fillW, bh - 4, col);
  }
  if (usbPresent) {
    int16_t mx = bx + bw / 2;
    c.drawLine(mx + 1, by + 2, mx - 2, centerY, COL_BG);
    c.drawLine(mx - 2, centerY, mx + 2, centerY, COL_BG);
    c.drawLine(mx + 2, centerY, mx - 1, by + bh - 3, COL_BG);
  }
  drawTextRight(c, fontAscii14(), bx - 6, centerY + 6, pctStr, col, COL_BG);
}

inline void drawPagePills(NovaCanvas& c, int16_t centerX, int16_t centerY,
                          int activeDot, int totalDots) {
  if (totalDots <= 0) return;
  const int16_t activeW = 18;
  const int16_t dotW = 5;
  const int16_t gap = 7;
  int16_t totalW = (totalDots - 1) * (dotW + gap) + activeW;
  int16_t px = centerX - totalW / 2;
  for (int i = 0; i < totalDots; i++) {
    bool act = (i == activeDot);
    int16_t w = act ? activeW : dotW;
    uint16_t col = act ? COL_ACCENT : COL_MUTED;
    aaFillRoundRect(c, px, centerY - 2, w, 4, 2, col, col);
    px += w + gap;
  }
}

inline int16_t burnOffsetX(uint8_t step) {
  static constexpr int8_t DX[5] = { 0, 1, 2, -1, -2 };
  return DX[step % 5];
}

inline int16_t burnOffsetY(uint8_t step) {
  static constexpr int8_t DY[5] = { 0, 1, -1, 2, -2 };
  return DY[(step / 5) % 5];
}

inline void drawTopBar(NovaCanvas& c, const NovaState& st, int activeDot, int totalDots = 4) {
  c.fillRect(0, 0, UI_W, 60, COL_BG);
  const int16_t ox = burnOffsetX(st.burnInStep);
  const int16_t oy = burnOffsetY(st.burnInStep);

  char clockBuf[8];
  snprintf(clockBuf, sizeof(clockBuf), "%02u:%02u", st.hour, st.minute);
  drawText(c, fontNum24(), UI_SAFE_L + 4 + ox, 41 + oy, clockBuf, COL_TEXT, COL_BG);

  uint16_t dotCol = COL_RED;
  uint16_t txtCol = COL_DIM;
  char netBuf[20] = "离线";
  if (st.wifiApMode && !st.wifiConnected) {
    dotCol = COL_AMBER;
    snprintf(netBuf, sizeof(netBuf), "配网");
  } else if (st.wifiConnected && st.mqttConnected) {
    bool stale = (st.lastMqttRxMs > 0 && st.nowMs > st.lastMqttRxMs && (st.nowMs - st.lastMqttRxMs) > 45000);
    bool waitingHost = (st.lastMqttRxMs == 0);
    if (stale) {
      dotCol = COL_AMBER;
      txtCol = COL_AMBER;
      uint32_t staleSec = (st.nowMs - st.lastMqttRxMs) / 1000;
      if (staleSec > 999) staleSec = 999;
      snprintf(netBuf, sizeof(netBuf), "滞后%us", (unsigned)staleSec);
    } else if (waitingHost) {
      dotCol = COL_AMBER;
      snprintf(netBuf, sizeof(netBuf), "等主机");
    } else {
      dotCol = COL_MINT;
      snprintf(netBuf, sizeof(netBuf), "在线");
    }
  } else if (st.wifiConnected) {
    dotCol = COL_AMBER;
    snprintf(netBuf, sizeof(netBuf), "无MQTT");
  }
  TextMetrics lm = measureText(c, fontCjk14(), netBuf);
  int16_t pillW = (int16_t)lm.w + 26;
  int16_t pillX = (int16_t)((UI_W - pillW) / 2 + 2 + ox);
  int16_t pillY = (int16_t)(16 + oy);
  aaFillRoundRect(c, pillX, pillY, pillW, 26, 13, COL_CARD, COL_BORDER);
  aaFillCircle(c, pillX + 11, pillY + 13, 3, dotCol);
  drawTextCenteredInRect(c, fontCjk14(), { (int16_t)(pillX + 16), pillY, (int16_t)(pillW - 20), 26 },
                         netBuf, txtCol, COL_CARD);

  drawBatteryIcon(c, UI_SAFE_R - 4 + ox, 29 + oy, st.battPct, st.usbPresent);
  drawPagePills(c, UI_W / 2 + ox, 52 + oy, activeDot, totalDots);
}

inline void fmtResetTime(uint32_t sec, char* out, size_t cap) {
  if (sec == 0) { snprintf(out, cap, "--"); return; }
  uint32_t d = sec / 86400;
  uint32_t h = (sec % 86400) / 3600;
  uint32_t m = (sec % 3600) / 60;
  if (d > 0) {
    if (h > 0) snprintf(out, cap, "%ud%uh", (unsigned)d, (unsigned)h);
    else snprintf(out, cap, "%ud", (unsigned)d);
  } else if (h > 0) {
    if (m > 0) snprintf(out, cap, "%uh%um", (unsigned)h, (unsigned)m);
    else snprintf(out, cap, "%uh", (unsigned)h);
  } else {
    snprintf(out, cap, "%um", (unsigned)m);
  }
}

}  // namespace ui
