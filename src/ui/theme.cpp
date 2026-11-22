#include "ui/theme.h"
#include <math.h>

extern const AaFont g_fontPf16;
extern const AaFont g_fontPf20;
extern const AaFont g_fontDin17;
extern const AaFont g_fontDin22;
extern const AaFont g_fontDin30;
extern const AaFont g_fontDin46;

namespace ui {

const AaFont* fontCjk14()   { return &g_fontPf16; }
const AaFont* fontCjk16()   { return &g_fontPf20; }
const AaFont* fontAscii14() { return &g_fontDin17; }
const AaFont* fontAscii18() { return &g_fontDin22; }
const AaFont* fontNum24()   { return &g_fontDin30; }
const AaFont* fontNum46()   { return &g_fontDin46; }

namespace {

inline uint16_t blendAlpha255(uint16_t bg, uint16_t fg, uint8_t a) {
  if (a == 0) return bg;
  if (a == 255) return fg;
  uint32_t inv = 255 - a;
  uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * inv + 127) / 255;
  uint32_t g = (((fg >> 5)  & 0x3F) * a + ((bg >> 5)  & 0x3F) * inv + 127) / 255;
  uint32_t b = ((fg         & 0x1F) * a + (bg         & 0x1F) * inv + 127) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

inline uint16_t blendAlpha15(uint16_t bg, uint16_t fg, uint8_t a) {
  if (a == 0) return bg;
  if (a >= 15) return fg;
  uint32_t inv = 15 - a;
  uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * inv + 7) / 15;
  uint32_t g = (((fg >> 5)  & 0x3F) * a + ((bg >> 5)  & 0x3F) * inv + 7) / 15;
  uint32_t b = ((fg         & 0x1F) * a + (bg         & 0x1F) * inv + 7) / 15;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

inline void blendPixel(uint16_t* fb, int16_t W, int16_t H, int16_t x, int16_t y, uint16_t col, uint8_t a) {
  if (x < 0 || y < 0 || x >= W || y >= H || a == 0) return;
  uint16_t& dst = fb[(int32_t)y * W + x];
  dst = blendAlpha255(dst, col, a);
}

uint16_t decodeUtf8Cp(const char*& p) {
  uint8_t b0 = (uint8_t)*p++;
  if (b0 < 0x80) return b0;
  if ((b0 & 0xE0) == 0xC0) {
    uint8_t b1 = (uint8_t)*p;
    if ((b1 & 0xC0) != 0x80) return '?';
    p++;
    return (uint16_t)(((b0 & 0x1F) << 6) | (b1 & 0x3F));
  }
  if ((b0 & 0xF0) == 0xE0) {
    uint8_t b1 = (uint8_t)p[0];
    uint8_t b2 = b1 ? (uint8_t)p[1] : 0;
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return '?';
    p += 2;
    return (uint16_t)(((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F));
  }
  if ((b0 & 0xF8) == 0xF0) {
    for (int i = 0; i < 3 && (*p & 0xC0) == 0x80; i++) p++;
    return '?';
  }
  return '?';
}

const AaGlyph* lookupGlyph(const AaFont* f, uint16_t cp, const AaFont** outFont) {
  for (const AaFont* cur = f; cur != nullptr; cur = cur->fallback) {
    int32_t lo = 0;
    int32_t hi = (int32_t)cur->glyphCount - 1;
    while (lo <= hi) {
      int32_t mid = (lo + hi) >> 1;
      uint16_t mcp = cur->glyphs[mid].cp;
      if (mcp == cp) {
        if (outFont) *outFont = cur;
        return &cur->glyphs[mid];
      }
      if (mcp < cp) lo = mid + 1;
      else hi = mid - 1;
    }
  }
  return nullptr;
}

inline uint8_t clamp255(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 255;
  return (uint8_t)(v * 255.0f + 0.5f);
}

}  // namespace

TextMetrics measureText(NovaCanvas& /*c*/, const AaFont* font, const char* s) {
  TextMetrics m{ 0, 0, 0, 0 };
  if (!font || !s || !s[0]) return m;

  int32_t curX = 0;
  int32_t minX = 0x7FFF, minY = 0x7FFF, maxX = -0x7FFF, maxY = -0x7FFF;
  bool hasInk = false;

  const char* p = s;
  while (*p) {
    uint16_t cp = decodeUtf8Cp(p);
    const AaFont* usedFont = nullptr;
    const AaGlyph* g = lookupGlyph(font, cp, &usedFont);
    if (!g) {
      curX += font->lineHeight / 2;
      continue;
    }
    if (g->w > 0 && g->h > 0) {
      int32_t gx0 = curX + g->xOff;
      int32_t gy0 = g->yOff;
      int32_t gx1 = gx0 + g->w;
      int32_t gy1 = gy0 + g->h;
      if (gx0 < minX) minX = gx0;
      if (gy0 < minY) minY = gy0;
      if (gx1 > maxX) maxX = gx1;
      if (gy1 > maxY) maxY = gy1;
      hasInk = true;
    }
    curX += g->xAdv;
  }

  if (hasInk) {
    m.x1 = (int16_t)minX;
    m.y1 = (int16_t)minY;
    m.w  = (uint16_t)(maxX - minX);
    m.h  = (uint16_t)(maxY - minY);
  } else {
    m.w = (uint16_t)curX;
  }
  return m;
}

void drawText(NovaCanvas& c, const AaFont* font, int16_t x, int16_t baselineY,
              const char* s, uint16_t fg, uint16_t /*bg*/) {
  if (!font || !s || !s[0]) return;
  uint16_t* fb = c.getFramebuffer();
  if (!fb) return;
  const int16_t W = (int16_t)c.width();
  const int16_t H = (int16_t)c.height();

  int32_t curX = x;
  const char* p = s;
  while (*p) {
    uint16_t cp = decodeUtf8Cp(p);
    const AaFont* usedFont = nullptr;
    const AaGlyph* g = lookupGlyph(font, cp, &usedFont);
    if (!g) {
      curX += font->lineHeight / 2;
      continue;
    }
    if (g->w > 0 && g->h > 0) {
      const uint8_t* src = usedFont->data + g->dataOff;
      const int16_t dstX0 = (int16_t)(curX + g->xOff);
      const int16_t dstY0 = (int16_t)(baselineY + g->yOff);
      const uint16_t gw = g->w;
      const uint16_t gh = g->h;
      uint32_t pxIdx = 0;

      for (uint16_t row = 0; row < gh; row++) {
        int16_t py = dstY0 + (int16_t)row;
        if (py < 0 || py >= H) {
          pxIdx += gw;
          continue;
        }
        uint16_t* rowFb = fb + (int32_t)py * W;
        for (uint16_t col = 0; col < gw; col++) {
          uint8_t byte = src[pxIdx >> 1];
          uint8_t a4 = (pxIdx & 1) ? (byte & 0x0F) : (byte >> 4);
          pxIdx++;
          if (a4 == 0) continue;
          int16_t px = dstX0 + (int16_t)col;
          if (px < 0 || px >= W) continue;
          rowFb[px] = (a4 >= 15) ? fg : blendAlpha15(rowFb[px], fg, a4);
        }
      }
    }
    curX += g->xAdv;
  }
}

void aaFillCircle(NovaCanvas& c, int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  if (r <= 0) return;
  uint16_t* fb = c.getFramebuffer();
  if (!fb) { c.fillCircle(cx, cy, r, col); return; }
  const int16_t W = (int16_t)c.width();
  const int16_t H = (int16_t)c.height();
  const float rOuter = (float)r + 0.5f;

  for (int16_t dy = -r; dy <= r; dy++) {
    int16_t py = cy + dy;
    if (py < 0 || py >= H) continue;
    for (int16_t dx = -r; dx <= r; dx++) {
      int16_t px = cx + dx;
      if (px < 0 || px >= W) continue;
      float d = sqrtf((float)(dx * dx + dy * dy));
      if (d <= rOuter - 1.0f) {
        fb[(int32_t)py * W + px] = col;
      } else if (d < rOuter) {
        blendPixel(fb, W, H, px, py, col, clamp255(rOuter - d));
      }
    }
  }
}

void aaDrawCircle(NovaCanvas& c, int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  if (r <= 0) return;
  uint16_t* fb = c.getFramebuffer();
  if (!fb) { c.drawCircle(cx, cy, r, col); return; }
  const int16_t W = (int16_t)c.width();
  const int16_t H = (int16_t)c.height();
  const float rf = (float)r;

  // Scan thin annulus in quadrant [0..r] and mirror 4 ways (~160 pixels for r=78).
  for (int16_t dy = 0; dy <= r + 1; dy++) {
    float maxDx = sqrtf(fmaxf(0.0f, (rf + 1.0f) * (rf + 1.0f) - (float)dy * (float)dy));
    float minDx = ((float)dy >= rf - 1.0f) ? 0.0f
                  : sqrtf(fmaxf(0.0f, (rf - 1.0f) * (rf - 1.0f) - (float)dy * (float)dy));
    int16_t xStart = (int16_t)floorf(minDx);
    int16_t xEnd   = (int16_t)ceilf(maxDx);
    for (int16_t dx = xStart; dx <= xEnd; dx++) {
      float d = fabsf(sqrtf((float)(dx * dx + dy * dy)) - rf);
      if (d >= 1.0f) continue;
      uint8_t a = clamp255(1.0f - d);
      blendPixel(fb, W, H, cx + dx, cy + dy, col, a);
      if (dx > 0) blendPixel(fb, W, H, cx - dx, cy + dy, col, a);
      if (dy > 0) blendPixel(fb, W, H, cx + dx, cy - dy, col, a);
      if (dx > 0 && dy > 0) blendPixel(fb, W, H, cx - dx, cy - dy, col, a);
    }
  }
}

void aaFillRoundRect(NovaCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                     int16_t r, uint16_t fill, uint16_t border) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r <= 1) {
    c.fillRect(x, y, w, h, fill);
    if (border != fill) c.drawRect(x, y, w, h, border);
    return;
  }

  uint16_t* fb = c.getFramebuffer();
  if (!fb) {
    c.fillRoundRect(x, y, w, h, r, fill);
    if (border != fill) c.drawRoundRect(x, y, w, h, r, border);
    return;
  }

  const int16_t W = (int16_t)c.width();
  const int16_t H = (int16_t)c.height();
  const bool hasBorder = (border != fill);

  // Fast solid interior bands (zero sqrt overhead).
  c.fillRect(x + r, y + 1, w - 2 * r, h - 2, fill);
  c.fillRect(x + 1, y + r, r - 1, h - 2 * r, fill);
  c.fillRect(x + w - r, y + r, r - 1, h - 2 * r, fill);

  if (hasBorder) {
    c.drawFastHLine(x + r, y, w - 2 * r, border);
    c.drawFastHLine(x + r, y + h - 1, w - 2 * r, border);
    c.drawFastVLine(x, y + r, h - 2 * r, border);
    c.drawFastVLine(x + w - 1, y + r, h - 2 * r, border);
  } else {
    c.drawFastHLine(x + r, y, w - 2 * r, fill);
    c.drawFastHLine(x + r, y + h - 1, w - 2 * r, fill);
    c.drawFastVLine(x, y + r, h - 2 * r, fill);
    c.drawFastVLine(x + w - 1, y + r, h - 2 * r, fill);
  }

  // Smooth AA corners over r x r quadrants. Center sits at (r-1, r-1) so d == r-1 aligns with flat edges.
  const float rCenter = (float)(r - 1);
  const float rOuterEdge = rCenter + 0.5f;
  const float rInnerEdge = rCenter - 0.5f;
  for (int16_t cy = 0; cy < r; cy++) {
    for (int16_t cx = 0; cx < r; cx++) {
      float dx = (float)(r - 1 - cx);
      float dy = (float)(r - 1 - cy);
      float d = sqrtf(dx * dx + dy * dy);
      if (d > rOuterEdge + 0.5f) continue;

      int16_t xs[4] = { (int16_t)(x + cx), (int16_t)(x + w - 1 - cx),
                        (int16_t)(x + cx), (int16_t)(x + w - 1 - cx) };
      int16_t ys[4] = { (int16_t)(y + cy), (int16_t)(y + cy),
                        (int16_t)(y + h - 1 - cy), (int16_t)(y + h - 1 - cy) };

      if (!hasBorder) {
        uint8_t a = (d <= rInnerEdge) ? 255 : clamp255(rOuterEdge - d + 0.5f);
        for (int q = 0; q < 4; q++) blendPixel(fb, W, H, xs[q], ys[q], fill, a);
      } else if (d <= rInnerEdge - 0.5f) {
        for (int q = 0; q < 4; q++) blendPixel(fb, W, H, xs[q], ys[q], fill, 255);
      } else if (d <= rCenter) {
        float t = d - (rInnerEdge - 0.5f);
        uint16_t mixed = blendAlpha255(fill, border, clamp255(t));
        for (int q = 0; q < 4; q++) blendPixel(fb, W, H, xs[q], ys[q], mixed, 255);
      } else {
        uint8_t a = clamp255(rOuterEdge - d + 0.5f);
        for (int q = 0; q < 4; q++) blendPixel(fb, W, H, xs[q], ys[q], border, a);
      }
    }
  }
}

void aaDrawRoundRect(NovaCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                     int16_t r, uint16_t border) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r <= 1) { c.drawRect(x, y, w, h, border); return; }

  uint16_t* fb = c.getFramebuffer();
  if (!fb) { c.drawRoundRect(x, y, w, h, r, border); return; }

  const int16_t W = (int16_t)c.width();
  const int16_t H = (int16_t)c.height();
  c.drawFastHLine(x + r, y, w - 2 * r, border);
  c.drawFastHLine(x + r, y + h - 1, w - 2 * r, border);
  c.drawFastVLine(x, y + r, h - 2 * r, border);
  c.drawFastVLine(x + w - 1, y + r, h - 2 * r, border);

  const float rf = (float)(r - 1);
  for (int16_t cy = 0; cy < r; cy++) {
    for (int16_t cx = 0; cx < r; cx++) {
      float dx = (float)(r - 1 - cx);
      float dy = (float)(r - 1 - cy);
      float d = fabsf(sqrtf(dx * dx + dy * dy) - rf);
      if (d >= 1.0f) continue;
      uint8_t a = clamp255(1.0f - d);
      blendPixel(fb, W, H, x + cx,         y + cy,         border, a);
      blendPixel(fb, W, H, x + w - 1 - cx, y + cy,         border, a);
      blendPixel(fb, W, H, x + cx,         y + h - 1 - cy, border, a);
      blendPixel(fb, W, H, x + w - 1 - cx, y + h - 1 - cy, border, a);
    }
  }
}

void drawSwitch(NovaCanvas& c, Rect r, bool on, uint16_t accent) {
  if (r.w <= 0 || r.h <= 0) return;
  int16_t rad = r.h / 2;
  uint16_t trackBg = on ? accent : COL_TRACK;
  uint16_t trackBd = on ? accent : COL_BORDER;
  uint16_t knobCol = on ? COL_TEXT : COL_DIM;
  aaFillRoundRect(c, r.x, r.y, r.w, r.h, rad, trackBg, trackBd);

  int16_t knobR = (r.h >= 28) ? 12 : (r.h - 6) / 2;
  if (knobR < 4) knobR = 4;
  int16_t cy = r.y + r.h / 2;
  int16_t cx = on ? (r.x + r.w - rad) : (r.x + rad);
  aaFillCircle(c, cx, cy, knobR, knobCol);
}

void drawSegmented(NovaCanvas& c, Rect r, const char* const* labels, int n, int sel,
                   uint16_t accent, const AaFont* f) {
  if (n <= 0 || !labels || r.w <= 8 || r.h <= 8) return;
  const AaFont* font = f ? f : fontCjk14();
  aaFillRoundRect(c, r.x, r.y, r.w, r.h, 11, COL_TRACK, COL_BORDER);

  const int16_t pad = 3;
  const int16_t innerW = r.w - 2 * pad;
  const int16_t segH = r.h - 2 * pad;
  for (int i = 0; i < n; i++) {
    int16_t x0 = (int16_t)(r.x + pad + (int32_t)i * innerW / n);
    int16_t x1 = (int16_t)(r.x + pad + (int32_t)(i + 1) * innerW / n);
    int16_t sw = x1 - x0;
    if (i > 0 && i != sel && (i - 1) != sel) {
      c.drawFastVLine(x0, r.y + 7, r.h - 14, COL_BORDER);
    }
    Rect segR{ x0, (int16_t)(r.y + pad), sw, segH };
    if (i == sel) {
      aaFillRoundRect(c, segR.x, segR.y, segR.w, segR.h, 8, accent, accent);
      drawTextCenteredInRect(c, font, segR, labels[i], COL_BG, accent);
    } else {
      drawTextCenteredInRect(c, font, segR, labels[i], COL_DIM, COL_TRACK);
    }
  }
}

void drawLevelBar(NovaCanvas& c, Rect r, int levels, int cur, uint16_t accent,
                  bool firstIsMute) {
  if (levels <= 0 || r.w <= 8 || r.h <= 8) return;
  if (cur < 0) cur = 0;
  if (cur >= levels) cur = levels - 1;

  aaFillRoundRect(c, r.x, r.y, r.w, r.h, 11, COL_TRACK, COL_BORDER);

  const int16_t pad = 3;
  const int16_t gap = 3;
  const int16_t innerW = r.w - 2 * pad - (levels - 1) * gap;
  const int16_t segH = r.h - 2 * pad;
  if (innerW <= levels) return;

  for (int i = 0; i < levels; i++) {
    int16_t x0 = (int16_t)(r.x + pad + (int32_t)i * innerW / levels + i * gap);
    int16_t x1 = (int16_t)(r.x + pad + (int32_t)(i + 1) * innerW / levels + i * gap);
    int16_t sw = x1 - x0;

    uint16_t bg, bd, fg;
    if (firstIsMute && i == 0) {
      if (cur == 0) {
        bg = COL_RED;
        bd = COL_RED;
        fg = COL_BG;
      } else {
        bg = COL_CARD_HI;
        bd = COL_BORDER;
        fg = COL_MUTED;
      }
    } else if (i == cur) {
      bg = accent;
      bd = accent;
      fg = COL_BG;
    } else if (i < cur && (!firstIsMute || i > 0)) {
      bg = blend565(COL_TRACK, accent, 145);
      bd = blend565(COL_BORDER, accent, 175);
      fg = accent;
    } else {
      bg = COL_CARD_HI;
      bd = COL_BORDER;
      fg = COL_DIM;
    }

    aaFillRoundRect(c, x0, r.y + pad, sw, segH, 7, bg, bd);
    if (firstIsMute && i == 0) {
      drawTextCenteredInRect(c, fontCjk14(), { x0, (int16_t)(r.y + pad), sw, segH },
                             "静音", fg, bg);
    }
  }
}

void drawSectionLabel(NovaCanvas& c, int16_t x, int16_t baseY, const char* s) {
  drawText(c, fontCjk14(), x, baseY, s, COL_DIM, COL_CARD);
}

void drawPressedOverlay(NovaCanvas& c, Rect r, int16_t radius) {
  if (r.w <= 0 || r.h <= 0) return;
  uint16_t* fb = c.getFramebuffer();
  if (!fb) return;

  const int16_t W = (int16_t)c.width();
  const int16_t H = (int16_t)c.height();
  int16_t rad = radius;
  if (rad * 2 > r.w) rad = r.w / 2;
  if (rad * 2 > r.h) rad = r.h / 2;
  if (rad < 0) rad = 0;

  int16_t x0 = r.x < 0 ? 0 : r.x;
  int16_t y0 = r.y < 0 ? 0 : r.y;
  int16_t x1 = (r.x + r.w) > W ? W : (r.x + r.w);
  int16_t y1 = (r.y + r.h) > H ? H : (r.y + r.h);
  const int32_t rMaxSq = (rad > 1) ? (int32_t)rad * (int32_t)rad : 0;

  for (int16_t py = y0; py < y1; py++) {
    int16_t ly = py - r.y;
    int16_t dy = 0;
    if (rad > 1) {
      if (ly < rad) dy = rad - 1 - ly;
      else if (ly >= r.h - rad) dy = ly - (r.h - rad);
    }
    uint16_t* row = fb + (int32_t)py * W;
    for (int16_t px = x0; px < x1; px++) {
      if (dy > 0) {
        int16_t lx = px - r.x;
        int16_t dx = 0;
        if (lx < rad) dx = rad - 1 - lx;
        else if (lx >= r.w - rad) dx = lx - (r.w - rad);
        if (dx > 0 && (int32_t)dx * dx + (int32_t)dy * dy > rMaxSq) continue;
      }
      row[px] = blend565(row[px], 0xFFFF, 46);  // +18% brightness feedback
    }
  }
}

}  // namespace ui
