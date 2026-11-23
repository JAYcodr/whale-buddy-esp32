#include "ui/pages.h"
#include "ui/easing.h"
#include "ui/theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int16_t kStageTop    = 148;
constexpr int16_t kStageH      = 194;
// 鱼真正活动的纵向范围：kStageCy ± (kWaveAmp + wob 2.2 + bob 2.6 + 半高 10.4 + glow 10)
// ≈ 195.8 .. 280.2。以前每帧清+推整个 194px 舞台，等于白搬一半像素——
// QSPI 实测有效带宽只有 2.7 MB/s，那一半就是 27 ms。
constexpr int16_t kPushTop     = 194;
constexpr int16_t kPushH       = 88;
constexpr float   kStageSpan   = 336.0f;
constexpr float   kStageX0     = 16.0f;
constexpr float   kStageCy     = 238.0f;
constexpr float   kWaveAmp     = 17.0f;
constexpr float   kWaveTurns   = 1.15f;
constexpr float   kPi          = 3.14159265f;

constexpr uint32_t kSwimStaggerMs  = 55u;
constexpr uint32_t kSwimItemDurMs  = 750u;
constexpr uint32_t kPowerSaveBobMs = 5000u;

constexpr uint16_t kColToday = rgb565(99, 212, 255);
constexpr uint16_t kColPeak  = rgb565(62, 134, 196);
constexpr uint16_t kColFish  = rgb565(33, 72, 109);
constexpr uint16_t kColEye   = rgb565(7, 12, 18);

int      s_selIdx       = -1;
uint32_t s_enterMs      = 0;
uint32_t s_animAnchorMs = 0;

struct WhaleGeom {
  float cx;
  float cy;
  float w;
  float h;
  float alpha;
};

inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

inline uint8_t clampByte(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 255.0f) return 255;
  return (uint8_t)(v + 0.5f);
}

// Minimum distance from point (px, py) to line segment (ax, ay)-(bx, by).
inline float distToSeg(float px, float py, float ax, float ay, float bx, float by) {
  float vx = bx - ax;
  float vy = by - ay;
  float wx = px - ax;
  float wy = py - ay;
  float c1 = vx * wx + vy * wy;
  if (c1 <= 0.0f) return sqrtf(wx * wx + wy * wy);
  float c2 = vx * vx + vy * vy;
  if (c2 <= c1) {
    float dx = px - bx;
    float dy = py - by;
    return sqrtf(dx * dx + dy * dy);
  }
  float t = c1 / c2;
  float dx = px - (ax + t * vx);
  float dy = py - (ay + t * vy);
  return sqrtf(dx * dx + dy * dy);
}

// Signed distance to a triangle (negative inside, positive outside).
float sdTriangle(float px, float py,
                 float ax, float ay, float bx, float by, float cx, float cy) {
  float e0x = bx - ax, e0y = by - ay;
  float e1x = cx - bx, e1y = cy - by;
  float e2x = ax - cx, e2y = ay - cy;

  float v0x = px - ax, v0y = py - ay;
  float v1x = px - bx, v1y = py - by;
  float v2x = px - cx, v2y = py - cy;

  float d0 = distToSeg(px, py, ax, ay, bx, by);
  float d1 = distToSeg(px, py, bx, by, cx, cy);
  float d2 = distToSeg(px, py, cx, cy, ax, ay);
  float minD = d0 < d1 ? d0 : d1;
  if (d2 < minD) minD = d2;

  float s0 = e0x * v0y - e0y * v0x;
  float s1 = e1x * v1y - e1y * v1x;
  float s2 = e2x * v2y - e2y * v2x;
  bool inside = ((s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f) ||
                 (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f));
  return inside ? -minD : minD;
}

bool shouldBobContinually(const NovaSettings& cfg, bool usbPresent) {
  if (cfg.powerProfile == PowerProfile::LIVELY) return true;
  if (cfg.powerProfile == PowerProfile::LONG_BATT) return false;
  return usbPresent;  // ADAPTIVE: continual on USB, timed power-save on battery.
}

uint32_t totalSwimDuration(uint8_t nDays) {
  if (nDays == 0) return 0;
  return (uint32_t)(nDays - 1) * kSwimStaggerMs + kSwimItemDurMs;
}

uint64_t findMaxDayTokens(const NovaState& st) {
  uint64_t maxV = 1;
  for (uint8_t i = 0; i < st.nTokenDays && i < MAX_TOKEN_DAYS; i++) {
    if (st.tokenDays[i].tokens > maxV) maxV = st.tokenDays[i].tokens;
  }
  return maxV;
}

WhaleGeom computeWhaleGeom(const NovaState& st, uint8_t i, uint64_t maxV,
                           uint32_t nowMs, float bobScale) {
  const uint8_t n = st.nTokenDays;
  const float v = (float)st.tokenDays[i].tokens;
  const float t = sqrtf(clampf(v / (float)maxV, 0.0f, 1.0f));
  float w = 13.0f + t * 23.0f;
  float h = w * 0.58f;

  const float slot = kStageSpan / (float)n;
  float cx = kStageX0 + slot * ((float)i + 0.5f) + sinf((float)i * 2.7f + 1.1f) * 2.6f;
  float phase = (n > 1) ? ((float)i / (float)(n - 1)) * kWaveTurns * 2.0f * kPi : 0.0f;
  float wob = sinf((float)i * 1.9f + 0.6f) * 2.2f;
  float cy = kStageCy + sinf(phase) * kWaveAmp + wob;

  float alpha = 1.0f;
  uint32_t swimElapsed = (nowMs >= s_enterMs) ? (nowMs - s_enterMs) : 0u;
  uint32_t delay = (uint32_t)i * kSwimStaggerMs;
  if (swimElapsed < delay + kSwimItemDurMs) {
    if (swimElapsed <= delay) {
      alpha = 0.0f;
    } else {
      float u = (float)(swimElapsed - delay) / (float)kSwimItemDurMs;
      float ease = ease::outCubic(u);
      cx -= (1.0f - ease) * 140.0f;
      float sc = 0.55f + 0.45f * ease;
      w *= sc;
      h *= sc;
      alpha = clampf(u / 0.6f, 0.0f, 1.0f);
    }
  }

  if (bobScale > 0.0f && alpha > 0.0f) {
    float period = 3.6f + (float)(i % 5) * 0.35f;
    float bobPhase = ((float)nowMs * 0.001f + (float)i * 0.73f) * (2.0f * kPi / period);
    cy += sinf(bobPhase) * 2.6f * bobScale;
  }

  return WhaleGeom{ cx, cy, w, h, alpha };
}

void drawWhale(NovaCanvas& c, const WhaleGeom& g, uint16_t col,
               bool isToday, bool isPeak, bool isSelected) {
  if (g.alpha <= 0.01f || g.w < 4.0f || g.h < 3.0f) return;
  uint16_t* fb = c.getFramebuffer();
  if (!fb) return;

  const int16_t W = (int16_t)c.width();
  const int16_t H = (int16_t)c.height();
  const float x0 = g.cx - g.w * 0.5f;
  const float y0 = g.cy - g.h * 0.5f;
  const float glowPad = isSelected ? 10.0f : (isToday ? 8.0f : (isPeak ? 4.0f : 1.0f));

  int16_t px0 = (int16_t)floorf(x0 - glowPad);
  int16_t py0 = (int16_t)floorf(y0 - glowPad);
  int16_t px1 = (int16_t)ceilf(x0 + g.w + glowPad);
  int16_t py1 = (int16_t)ceilf(y0 + g.h + glowPad);
  if (px0 < 0) px0 = 0;
  if (py0 < kStageTop) py0 = kStageTop;
  if (px1 >= W) px1 = W - 1;
  if (py1 >= kStageTop + kStageH) py1 = kStageTop + kStageH - 1;

  // Body ellipse parameters matching whale-school.html silhouette.
  const float bodyCx = x0 + g.w * 0.455f;
  const float bodyCy = y0 + g.h * 0.50f;
  const float bodyRx = g.w * 0.405f;
  const float bodyRy = g.h * 0.43f;
  const float invRx2 = 1.0f / (bodyRx * bodyRx);
  const float invRy2 = 1.0f / (bodyRy * bodyRy);
  const float bodyScale = (bodyRx < bodyRy ? bodyRx : bodyRy);

  // Belly highlight ellipse.
  const float bellyCx = x0 + g.w * 0.44f;
  const float bellyCy = y0 + g.h * 0.74f;
  const float bellyInvRx2 = 1.0f / ((g.w * 0.28f) * (g.w * 0.28f));
  const float bellyInvRy2 = 1.0f / ((g.h * 0.12f) * (g.h * 0.12f));
  const uint16_t bellyCol = ui::blend565(col, 0xFFFF, 34);

  // Eye circle.
  const float eyeCx = x0 + g.w * 0.24f;
  const float eyeCy = y0 + g.h * 0.43f;
  const float eyeR  = (g.w * 0.062f > 1.0f) ? (g.w * 0.062f) : 1.0f;

  // Tail fluke two overlapping triangles + dorsal fin triangle.
  const float tx0 = x0 + g.w * 0.78f, ty0 = y0 + g.h * 0.48f;
  const float tx1 = x0 + g.w * 1.00f, ty1 = y0 + g.h * 0.06f;
  const float tx2 = x0 + g.w * 0.90f, ty2 = y0 + g.h * 0.50f;
  const float tx3 = x0 + g.w * 1.00f, ty3 = y0 + g.h * 0.94f;
  const float fx0 = x0 + g.w * 0.30f, fy0 = y0 + g.h * 0.12f;
  const float fx1 = x0 + g.w * 0.42f, fy1 = y0 + g.h * 0.01f;
  const float fx2 = x0 + g.w * 0.46f, fy2 = y0 + g.h * 0.16f;

  const float maxGlowStrength = isSelected ? 0.55f : (isToday ? 0.42f : (isPeak ? 0.16f : 0.0f));

  for (int16_t py = py0; py <= py1; py++) {
    uint16_t* row = fb + (int32_t)py * W;
    const float fy = (float)py + 0.5f;
    const float dyB = fy - bodyCy;
    const float dyNorm2 = dyB * dyB * invRy2;

    for (int16_t px = px0; px <= px1; px++) {
      const float fx = (float)px + 0.5f;
      const float dxB = fx - bodyCx;
      const float ell2 = dxB * dxB * invRx2 + dyNorm2;

      // 椭圆 2 倍半径之外、又不在尾鳍横坐标范围内的像素一定为空
      // （辉光只到 1.9 倍半径）。包围盒四角靠这一条直接跳过。
      if (ell2 > 4.0f && fx < x0 + g.w * 0.72f) continue;

      const float ellNorm = sqrtf(ell2);
      const float sdBody = (ellNorm - 1.0f) * bodyScale;

      float sd = sdBody;
      // 椭圆内部的像素 sdBody 已经是最小距离，三角形不可能更近——
      // 以前对每个像素都白算 2~3 次点到线段距离，这是 40 ms 帧的主因。
      if (sdBody >= 0.75f) {
        if (fx > x0 + g.w * 0.72f) {
          float sdT1 = sdTriangle(fx, fy, tx0, ty0, tx1, ty1, tx2, ty2);
          float sdT2 = sdTriangle(fx, fy, tx0, ty0, tx2, ty2, tx3, ty3);
          if (sdT1 < sd) sd = sdT1;
          if (sdT2 < sd) sd = sdT2;
        }
        if (fy < bodyCy && fx > x0 + g.w * 0.24f && fx < x0 + g.w * 0.52f) {
          float sdFin = sdTriangle(fx, fy, fx0, fy0, fx1, fy1, fx2, fy2);
          if (sdFin < sd) sd = sdFin;
        }
      }

      if (sd < 0.75f) {
        float cov = clampf(0.5f - sd, 0.0f, 1.0f) * g.alpha;
        uint16_t fg = col;

        // Subtle belly highlight inside body.
        if (sdBody < -0.5f && fy > bodyCy) {
          float bdx = fx - bellyCx;
          float bdy = fy - bellyCy;
          if (bdx * bdx * bellyInvRx2 + bdy * bdy * bellyInvRy2 <= 1.0f) {
            fg = bellyCol;
          }
        }
        // Anti-aliased dark eye —— 先比平方距离，只有真落在眼睛附近才开方。
        float edx = fx - eyeCx;
        float edy = fy - eyeCy;
        float ed2 = edx * edx + edy * edy;
        float eyeOuter = eyeR + 0.7f;
        if (ed2 < eyeOuter * eyeOuter) {
          float ed = sqrtf(ed2) - eyeR;
          float eyeCov = clampf(0.5f - ed, 0.0f, 1.0f);
          fg = ui::blend565(fg, kColEye, clampByte(eyeCov * 255.0f));
        }

        row[px] = ui::blend565(row[px], fg, clampByte(cov * 255.0f));
      } else if (maxGlowStrength > 0.0f && ellNorm < 1.9f) {
        float u = clampf(1.0f - (ellNorm - 1.0f) / 0.9f, 0.0f, 1.0f);
        float glowCov = maxGlowStrength * u * u * g.alpha;
        if (glowCov > 0.01f) {
          row[px] = ui::blend565(row[px], col, clampByte(glowCov * 255.0f));
        }
      }
    }
  }
}

void formatTokenCompact(uint64_t tok, char* numOut, size_t numCap, const char** unitOut) {
  if (tok >= 100000000ULL) {
    snprintf(numOut, numCap, "%.2f", (double)tok / 100000000.0);
    *unitOut = "亿";
  } else if (tok >= 10000000ULL) {
    uint64_t wan = (tok + 5000ULL) / 10000ULL;
    snprintf(numOut, numCap, "%llu", (unsigned long long)wan);
    *unitOut = "万";
  } else if (tok >= 10000ULL) {
    snprintf(numOut, numCap, "%.1f", (double)tok / 10000.0);
    *unitOut = "万";
  } else {
    snprintf(numOut, numCap, "%llu", (unsigned long long)tok);
    *unitOut = "";
  }
}

void formatExactTokens(uint64_t tok, char* out, size_t cap) {
  char raw[32];
  snprintf(raw, sizeof(raw), "%llu", (unsigned long long)tok);
  size_t len = strlen(raw);
  size_t pos = 0;
  for (size_t i = 0; i < len && pos + 1 < cap; i++) {
    if (i > 0 && ((len - i) % 3) == 0 && pos + 2 < cap) {
      out[pos++] = ',';
    }
    out[pos++] = raw[i];
  }
  out[pos] = '\0';
}

void drawStatCell(NovaCanvas& c, ui::Rect r, const char* label,
                  const char* val, const char* unit, uint16_t valCol) {
  ui::drawCard(c, r.x, r.y, r.w, r.h, ui::COL_CARD, ui::COL_BORDER, 12);
  ui::drawText(c, ui::fontCjk14(), r.x + 10, r.y + 21, label, ui::COL_DIM, ui::COL_CARD);
  ui::drawText(c, ui::fontAscii18(), r.x + 10, r.y + 47, val, valCol, ui::COL_CARD);
  if (unit && unit[0]) {
    ui::TextMetrics vm = ui::measureText(c, ui::fontAscii18(), val);
    ui::drawText(c, ui::fontCjk14(), (int16_t)(r.x + 12 + (int16_t)vm.w), r.y + 46,
                 unit, ui::COL_DIM, ui::COL_CARD);
  }
}

float computeBobScale(const NovaState& st, const NovaSettings& cfg, uint32_t nowMs) {
  if (st.nTokenDays == 0) return 0.0f;
  if (shouldBobContinually(cfg, st.usbPresent)) return 1.0f;
  uint32_t swimEnd = totalSwimDuration(st.nTokenDays);
  uint32_t sinceEnter = (nowMs >= s_enterMs) ? (nowMs - s_enterMs) : 0u;
  uint32_t sinceAnchor = (nowMs >= s_animAnchorMs) ? (nowMs - s_animAnchorMs) : 0u;
  uint32_t totalBobWindow = (kPowerSaveBobMs > swimEnd) ? kPowerSaveBobMs : (swimEnd + 1500u);
  if (sinceEnter < swimEnd) return 1.0f;
  if (sinceAnchor >= totalBobWindow) return 0.0f;
  uint32_t rem = totalBobWindow - sinceAnchor;
  // Ease bob amplitude gently to 0 over the final 1000ms before power-save rest.
  return (rem < 1000u) ? ((float)rem / 1000.0f) : 1.0f;
}

void drawWhaleSchool(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t nowMs) {
  // COL_BG 是纯黑 0x0000：memset 比逐像素 fillRect 快一个数量级
  // （fillRect 在 80 MHz + PSRAM canvas 上约 0.5 µs/px）。
  uint16_t* fb = c.getFramebuffer();
  if (fb) {
    memset(fb + (int32_t)kPushTop * UI_W, 0, sizeof(uint16_t) * UI_W * kPushH);
  }
  if (st.nTokenDays == 0) return;

  const uint64_t maxV = findMaxDayTokens(st);
  const float bobScale = computeBobScale(st, cfg, nowMs);
  const uint8_t n = (st.nTokenDays > MAX_TOKEN_DAYS) ? MAX_TOKEN_DAYS : st.nTokenDays;

  if (s_selIdx >= (int)n) s_selIdx = -1;

  for (uint8_t i = 0; i < n; i++) {
    bool isToday = (i == n - 1);
    bool isPeak  = (st.tokenDays[i].tokens == maxV);
    bool isSel   = ((int)i == s_selIdx);
    uint16_t col = isSel ? ui::COL_ACCENT : (isToday ? kColToday : (isPeak ? kColPeak : kColFish));
    WhaleGeom g = computeWhaleGeom(st, i, maxV, nowMs, bobScale);
    drawWhale(c, g, col, isToday, isPeak, isSel);
  }
}

int hitTestWhale(int16_t x, int16_t y, const NovaState& st, uint32_t nowMs) {
  if (st.nTokenDays == 0 || y < kStageTop || y >= kStageTop + kStageH) return -1;
  const uint64_t maxV = findMaxDayTokens(st);
  const uint8_t n = (st.nTokenDays > MAX_TOKEN_DAYS) ? MAX_TOKEN_DAYS : st.nTokenDays;
  int best = -1;
  float bestD2 = 1e9f;
  for (uint8_t i = 0; i < n; i++) {
    WhaleGeom g = computeWhaleGeom(st, i, maxV, nowMs, 0.0f);
    float rx = g.w * 0.65f + 8.0f;
    float ry = g.h * 0.70f + 14.0f;
    float dx = (float)x - g.cx;
    float dy = (float)y - g.cy;
    float norm2 = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry);
    if (norm2 <= 1.0f && norm2 < bestD2) {
      bestD2 = norm2;
      best = (int)i;
    }
  }
  return best;
}

}  // namespace

void pageTokensOnEnter(uint32_t nowMs) {
  s_enterMs = nowMs;
  s_animAnchorMs = nowMs;
  s_selIdx = -1;
}

bool pageTokensIsAnimating(const NovaState& st, const NovaSettings& cfg, uint32_t nowMs) {
  if (st.nTokenDays == 0) return false;
  uint32_t swimEnd = totalSwimDuration(st.nTokenDays);
  uint32_t sinceEnter = (nowMs >= s_enterMs) ? (nowMs - s_enterMs) : 0u;
  if (sinceEnter < swimEnd) return true;
  if (shouldBobContinually(cfg, st.usbPresent)) return true;
  uint32_t totalBobWindow = (kPowerSaveBobMs > swimEnd) ? kPowerSaveBobMs : (swimEnd + 1500u);
  uint32_t sinceAnchor = (nowMs >= s_animAnchorMs) ? (nowMs - s_animAnchorMs) : 0u;
  return sinceAnchor < totalBobWindow;
}

void pageTokensDrawStage(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg,
                         uint32_t nowMs) {
  drawWhaleSchool(c, st, cfg, nowMs);
}

void pageTokensDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  c.fillScreen(ui::COL_BG);
  ui::drawTopBar(c, st, 0);

  if (st.nTokenDays == 0) {
    ui::drawCard(c, 24, 128, 320, 140, ui::COL_CARD, ui::COL_BORDER, 18);
    ui::drawTextCentered(c, ui::fontCjk16(), UI_W / 2, 188,
                         "暂未收到 Token 统计", ui::COL_TEXT, ui::COL_CARD);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 222,
                         "等待宿主端推送每日鲸群用量", ui::COL_DIM, ui::COL_CARD);
    return;
  }

  const uint8_t n = (st.nTokenDays > MAX_TOKEN_DAYS) ? MAX_TOKEN_DAYS : st.nTokenDays;
  if (s_selIdx >= (int)n) s_selIdx = -1;

  // Hero metric: selected day or today's consumption.
  const int showIdx = (s_selIdx >= 0) ? s_selIdx : ((int)n - 1);
  const TokenDay& focusDay = st.tokenDays[showIdx];

  char eyebrow[32];
  uint16_t eyeCol = ui::COL_DIM;
  if (s_selIdx >= 0) {
    snprintf(eyebrow, sizeof(eyebrow), "%s · 当日消耗",
             focusDay.date[0] ? focusDay.date : "--");
    eyeCol = ui::COL_ACCENT;
  } else {
    snprintf(eyebrow, sizeof(eyebrow), "今日消耗");
  }
  ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 82, eyebrow, eyeCol, ui::COL_BG);

  char heroNum[24];
  const char* heroUnit = "";
  formatTokenCompact(focusDay.tokens, heroNum, sizeof(heroNum), &heroUnit);
  ui::TextMetrics nm = ui::measureText(c, ui::fontNum46(), heroNum);
  ui::TextMetrics um = heroUnit[0] ? ui::measureText(c, ui::fontCjk16(), heroUnit)
                                   : ui::TextMetrics{ 0, 0, 0, 0 };
  const int16_t gap = heroUnit[0] ? 6 : 0;
  const int16_t totalW = (int16_t)nm.w + gap + (int16_t)um.w;
  const int16_t startX = (int16_t)((UI_W - totalW) / 2 - nm.x1);
  const uint16_t heroCol = (s_selIdx >= 0) ? ui::COL_ACCENT : ui::COL_TEXT;
  ui::drawText(c, ui::fontNum46(), startX, 125, heroNum, heroCol, ui::COL_BG);
  if (heroUnit[0]) {
    ui::drawText(c, ui::fontCjk16(), (int16_t)(startX + (int16_t)nm.w + gap - um.x1), 123,
                 heroUnit, ui::COL_DIM, ui::COL_BG);
  }

  char subBuf[64];
  if (s_selIdx >= 0) {
    char exactBuf[32];
    formatExactTokens(focusDay.tokens, exactBuf, sizeof(exactBuf));
    snprintf(subBuf, sizeof(subBuf), "精确用量 %s tokens", exactBuf);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 145, subBuf, ui::COL_DIM, ui::COL_BG);
  } else {
    char totNum[24];
    const char* totUnit = "";
    formatTokenCompact(st.tokenTotal, totNum, sizeof(totNum), &totUnit);
    uint16_t span = st.tokenSpanDays ? st.tokenSpanDays : n;
    snprintf(subBuf, sizeof(subBuf), "累计 %s %s · %u 天 · 一条鱼 = 一天",
             totNum, totUnit, (unsigned)span);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 145, subBuf, ui::COL_MUTED, ui::COL_BG);
  }

  // Whale school wave.
  drawWhaleSchool(c, st, settingsGet(), nowMs);

  // Bottom 3 metric cards.
  const uint64_t maxV = findMaxDayTokens(st);
  uint64_t avgV = st.tokenAvgOverride;
  if (avgV == 0) {
    uint64_t sum = 0;
    for (uint8_t i = 0; i < n; i++) sum += st.tokenDays[i].tokens;
    avgV = sum / n;
  }

  char peakStr[16], avgStr[16], cacheStr[16];
  const char* peakUnit = "";
  const char* avgUnit  = "";
  formatTokenCompact(maxV, peakStr, sizeof(peakStr), &peakUnit);
  formatTokenCompact(avgV, avgStr, sizeof(avgStr), &avgUnit);
  snprintf(cacheStr, sizeof(cacheStr), "%.1f", st.tokenCachePct);

  drawStatCell(c, { 20,  352, 104, 58 }, "最肥的一天", peakStr, peakUnit, ui::COL_TEXT);
  drawStatCell(c, { 132, 352, 104, 58 }, "日均", avgStr, avgUnit, ui::COL_TEXT);
  drawStatCell(c, { 244, 352, 104, 58 }, "缓存命中", cacheStr, "%", ui::COL_ACCENT);

  char footBuf[64];
  if (s_selIdx >= 0) {
    snprintf(footBuf, sizeof(footBuf), "已选 %s · 再点或点上方恢复今日",
             focusDay.date[0] ? focusDay.date : "--");
  } else {
    snprintf(footBuf, sizeof(footBuf), "%u 条鲸一起游 · 点鲸鱼看当日用量", (unsigned)n);
  }
  ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 432, footBuf, ui::COL_MUTED, ui::COL_BG);
}

UiAction pageTokensTap(int16_t x, int16_t y, const NovaState& st, uint32_t nowMs) {
  if (st.nTokenDays == 0 || y < 60) return UiAction{};
  int hit = hitTestWhale(x, y, st, nowMs);
  if (hit >= 0) {
    s_selIdx = (s_selIdx == hit) ? -1 : hit;
    s_animAnchorMs = nowMs;
    return UiAction{ UiAction::TOKENS_REDRAW, s_selIdx };
  }
  if (s_selIdx >= 0) {
    s_selIdx = -1;
    s_animAnchorMs = nowMs;
    return UiAction{ UiAction::TOKENS_REDRAW, -1 };
  }
  return UiAction{};
}

bool pageTokensHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings&,
                       ui::Rect* out) {
  if (!out || st.nTokenDays == 0) return false;
  int hit = hitTestWhale(x, y, st, st.nowMs);
  if (hit >= 0) {
    WhaleGeom g = computeWhaleGeom(st, (uint8_t)hit, findMaxDayTokens(st), st.nowMs, 0.0f);
    int16_t pad = 6;
    *out = ui::Rect{
      (int16_t)(g.cx - g.w * 0.5f - pad),
      (int16_t)(g.cy - g.h * 0.5f - pad),
      (int16_t)(g.w + pad * 2),
      (int16_t)(g.h + pad * 2)
    };
    return true;
  }
  return false;
}

ui::Rect pageTokensStageRect() {
  return { 0, kPushTop, UI_W, kPushH };
}
