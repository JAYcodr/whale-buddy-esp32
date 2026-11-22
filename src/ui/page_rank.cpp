#include "ui/pages.h"
#include "ui/theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int16_t LIST_TOP       = 98;
constexpr int16_t LIST_BOTTOM    = 391;
constexpr int16_t ROW_H          = 54;
constexpr int16_t ROW_PITCH      = 59;
constexpr float   OVERSCROLL_MAX = 28.0f;
constexpr float   OVERSCROLL_DAMP = 0.4f;
constexpr uint32_t BOUNCE_MS     = 160;
constexpr float   FLING_DECAY    = 0.94f;
constexpr float   FLING_STOP_V   = 0.3f;
constexpr ui::Rect BTN_MODE{ 238, 63, 110, 28 };

float   s_scrollY    = 0.0f;
uint8_t s_viewMode   = 0;  // 0 = B/F/K metrics, 1 = Aesthetic/Maint/Duration/Cost
uint8_t s_lastNRows  = 0;
bool    s_hasScrolled = false;

struct TouchSample {
  int16_t  y;
  uint32_t ms;
};

TouchSample s_samples[3] = {};
uint8_t  s_sampleCount = 0;
int16_t  s_lastTouchY  = 0;
bool     s_touchActive = false;

bool     s_flinging    = false;
float    s_flingVel    = 0.0f;  // scrollY delta per 16.67ms frame
uint32_t s_lastTickMs  = 0;

bool     s_bouncing      = false;
float    s_bounceFrom    = 0.0f;
float    s_bounceTo      = 0.0f;
uint32_t s_bounceStartMs = 0;

uint16_t medalColor(uint8_t rank) {
  if (rank == 1) return ui::COL_GOLD;
  if (rank == 2) return ui::COL_SILVER;
  if (rank == 3) return ui::COL_BRONZE;
  return ui::COL_DIM;
}

int16_t maxScrollFor(uint8_t nRows) {
  int16_t viewH = LIST_BOTTOM - LIST_TOP;
  int16_t totalH = (int16_t)nRows * ROW_PITCH - 5;
  return (totalH > viewH) ? (totalH - viewH) : 0;
}

void startBounce(float target, uint32_t ms) {
  s_flinging = false;
  s_flingVel = 0.0f;
  s_bouncing = true;
  s_bounceFrom = s_scrollY;
  s_bounceTo = target;
  s_bounceStartMs = ms;
}

bool drawRankRowsAndScrollbar(NovaCanvas& c, const NovaState& st) {
  c.fillRect(0, LIST_TOP - 2, UI_W, LIST_BOTTOM - (LIST_TOP - 2), ui::COL_BG);

  int16_t scrollPx = (int16_t)lroundf(s_scrollY);
  bool topTouched = false;

  for (int i = 0; i < st.nRows && i < MAX_RANK_ROWS; i++) {
    const RankRow& row = st.rows[i];
    const int16_t rx = 18;
    const int16_t ry = LIST_TOP + i * ROW_PITCH - scrollPx;
    const int16_t rw = 326;
    const int16_t rh = ROW_H;

    if (ry + rh <= LIST_TOP - 2 || ry >= LIST_BOTTOM) continue;
    if (ry < 60) topTouched = true;

    bool active = (st.activeModel[0] && strstr(st.activeModel, row.m) != nullptr);
    uint16_t rowBg = active ? rgb565(12, 34, 34) : ui::COL_CARD;
    uint16_t rowBd = active ? rgb565(36, 128, 104) : ui::COL_BORDER;
    ui::aaFillRoundRect(c, rx, ry, rw, rh, 13, rowBg, rowBd);

    if (row.r <= 3) {
      uint16_t mc = medalColor(row.r);
      // Keep the 3px medal stripe inside the straight left edge (clear of r=13 corner curves).
      ui::aaFillRoundRect(c, rx + 2, ry + 12, 3, rh - 24, 1, mc, mc);
    }

    // Rank badge
    char rankStr[8];
    snprintf(rankStr, sizeof(rankStr), "%u", row.r);
    ui::Rect badgeR{ (int16_t)(rx + 8), (int16_t)(ry + 13), 28, 28 };
    if (row.r <= 3) {
      uint16_t mc = medalColor(row.r);
      ui::aaFillRoundRect(c, badgeR.x, badgeR.y, badgeR.w, badgeR.h, 8, mc, mc);
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), badgeR, rankStr, ui::COL_BG, mc);
    } else {
      ui::aaFillRoundRect(c, badgeR.x, badgeR.y, badgeR.w, badgeR.h, 8,
                          ui::COL_CARD_HI, ui::COL_CARD_HI);
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), badgeR, rankStr,
                                 ui::COL_DIM, ui::COL_CARD_HI);
    }

    // Score string measured upfront so model name gets maximum non-overlapping width.
    char scoreBuf[12];
    snprintf(scoreBuf, sizeof(scoreBuf), "%.1f", row.s);
    ui::TextMetrics scoreM = ui::measureText(c, ui::fontAscii18(), scoreBuf);

    // Top sub-row: model name + strength pill + optional NEW pill (pills reserved before score).
    const int16_t nameX = rx + 44;
    const int16_t scoreLimitX = rx + rw - 12 - (int16_t)scoreM.w - 8;
    const int16_t pillH = 18;
    ui::TextMetrics mg = ui::measureText(c, ui::fontCjk14(), "Mg");
    int16_t midY = (int16_t)((ry + 23) + mg.y1 + (int16_t)mg.h / 2);
    int16_t pillY = (int16_t)(midY - pillH / 2);

    bool hasE = (row.e[0] != '\0');
    int16_t ePillW = 0;
    if (hasE) {
      ui::TextMetrics em = ui::measureText(c, ui::fontAscii14(), row.e);
      ePillW = (int16_t)em.w + 8;
      if (ePillW < 26) ePillW = 26;
    }
    int16_t newPillW = 0;
    if (row.isNew) {
      ui::TextMetrics newM = ui::measureText(c, ui::fontAscii14(), "NEW");
      newPillW = (int16_t)newM.w + 8;
    }

    int16_t reserveW = (hasE ? (5 + ePillW) : 0) + (row.isNew ? (5 + newPillW) : 0);
    int16_t nameMaxW = scoreLimitX - nameX - reserveW;
    if (nameMaxW < 24) nameMaxW = 24;

    char modelBuf[36];
    ui::fitUtf8Line(c, ui::fontCjk14(), row.m, nameMaxW, modelBuf, sizeof(modelBuf));
    ui::drawText(c, ui::fontCjk14(), nameX, ry + 23, modelBuf,
                 active ? ui::COL_MINT : ui::COL_TEXT, rowBg);

    ui::TextMetrics nm = ui::measureText(c, ui::fontCjk14(), modelBuf);
    int16_t px = (int16_t)(nameX + (int16_t)nm.w + 5);
    if (hasE) {
      ui::aaFillRoundRect(c, px, pillY, ePillW, pillH, 5, ui::COL_CARD_HI, ui::COL_BORDER);
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), { px, pillY, ePillW, pillH },
                                 row.e, ui::COL_DIM, ui::COL_CARD_HI);
      px = (int16_t)(px + ePillW + 5);
    }
    if (row.isNew) {
      const uint16_t newBg = rgb565(18, 52, 46);
      const uint16_t newBd = rgb565(36, 128, 104);
      ui::aaFillRoundRect(c, px, pillY, newPillW, pillH, 5, newBg, newBd);
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), { px, pillY, newPillW, pillH },
                                 "NEW", ui::COL_MINT, newBg);
    }

    // Bottom sub-row: aligned numeric metrics (no progress bars)
    char numBuf[8];
    if (s_viewMode == 0) {
      ui::drawText(c, ui::fontAscii14(), rx + 44, ry + 45, "B", ui::COL_ACCENT, rowBg);
      snprintf(numBuf, sizeof(numBuf), "%u", row.b);
      ui::drawText(c, ui::fontAscii14(), rx + 57, ry + 45, numBuf, ui::COL_TEXT, rowBg);

      ui::drawText(c, ui::fontAscii14(), rx + 98, ry + 45, "F", ui::COL_MINT, rowBg);
      snprintf(numBuf, sizeof(numBuf), "%u", row.f);
      ui::drawText(c, ui::fontAscii14(), rx + 111, ry + 45, numBuf, ui::COL_TEXT, rowBg);

      ui::drawText(c, ui::fontAscii14(), rx + 152, ry + 45, "K", ui::COL_VIOLET, rowBg);
      snprintf(numBuf, sizeof(numBuf), "%u", row.k);
      ui::drawText(c, ui::fontAscii14(), rx + 165, ry + 45, numBuf, ui::COL_TEXT, rowBg);
    } else {
      ui::drawText(c, ui::fontAscii14(), rx + 44, ry + 45, "A", ui::COL_AMBER, rowBg);
      snprintf(numBuf, sizeof(numBuf), "%u", row.a);
      ui::drawText(c, ui::fontAscii14(), rx + 57, ry + 45, numBuf, ui::COL_TEXT, rowBg);

      ui::drawText(c, ui::fontAscii14(), rx + 98, ry + 45, "Q", ui::COL_VIOLET, rowBg);
      snprintf(numBuf, sizeof(numBuf), "%u", row.q);
      ui::drawText(c, ui::fontAscii14(), rx + 111, ry + 45, numBuf, ui::COL_TEXT, rowBg);

      char rawMeta[28], meta[28];
      if (row.dur[0] && row.cost[0]) {
        snprintf(rawMeta, sizeof(rawMeta), "%s %s", row.dur, row.cost);
      } else if (row.dur[0]) {
        snprintf(rawMeta, sizeof(rawMeta), "%s", row.dur);
      } else {
        snprintf(rawMeta, sizeof(rawMeta), "%s", row.cost[0] ? row.cost : "--");
      }
      ui::fitUtf8Line(c, ui::fontCjk14(), rawMeta, 104, meta, sizeof(meta));
      ui::drawText(c, ui::fontCjk14(), rx + 152, ry + 45, meta, ui::COL_DIM, rowBg);
    }

    // Score (DIN 22px right-aligned)
    ui::drawTextRight(c, ui::fontAscii18(), rx + rw - 12, ry + 35, scoreBuf,
                      row.r == 1 ? ui::COL_GOLD : (active ? ui::COL_MINT : ui::COL_TEXT), rowBg);
  }

  int16_t maxS = maxScrollFor(st.nRows);
  if (maxS > 0) {
    const int16_t sbX = 351;
    const int16_t sbY = LIST_TOP;
    const int16_t sbH = LIST_BOTTOM - LIST_TOP - 6;
    ui::aaFillRoundRect(c, sbX, sbY, 4, sbH, 2, ui::COL_TRACK, ui::COL_TRACK);
    int16_t thumbH = (int16_t)((int32_t)sbH * (LIST_BOTTOM - LIST_TOP) /
                               (st.nRows * ROW_PITCH));
    if (thumbH < 24) thumbH = 24;
    int16_t clampedScroll = scrollPx;
    if (clampedScroll < 0) clampedScroll = 0;
    if (clampedScroll > maxS) clampedScroll = maxS;
    int16_t thumbY = sbY + (int16_t)((int32_t)(sbH - thumbH) * clampedScroll / maxS);
    ui::aaFillRoundRect(c, sbX, thumbY, 4, thumbH, 2, ui::COL_ACCENT, ui::COL_ACCENT);
  }

  return topTouched;
}

void drawRankHeaderAndFooter(NovaCanvas& c, const NovaState& st, bool drawTopBar) {
  if (drawTopBar) {
    c.fillRect(0, 0, UI_W, LIST_TOP - 2, ui::COL_BG);
    ui::drawTopBar(c, st, 3);
  } else {
    c.fillRect(0, 60, UI_W, LIST_TOP - 2 - 60, ui::COL_BG);
  }
  c.fillRect(0, LIST_BOTTOM, UI_W, UI_H - LIST_BOTTOM, ui::COL_BG);

  int16_t maxS = maxScrollFor(st.nRows);
  int16_t clampedScroll = (int16_t)lroundf(s_scrollY);
  if (clampedScroll < 0) clampedScroll = 0;
  if (clampedScroll > maxS) clampedScroll = maxS;

  int firstVis = (clampedScroll + 12) / ROW_PITCH + 1;
  int lastVis = (clampedScroll + (LIST_BOTTOM - LIST_TOP) - 12) / ROW_PITCH + 1;
  if (firstVis < 1) firstVis = 1;
  if (lastVis > st.nRows) lastVis = st.nRows;
  if (lastVis < firstVis) lastVis = firstVis;

  char titleBuf[40];
  snprintf(titleBuf, sizeof(titleBuf), "编程榜 %d-%d/%u", firstVis, lastVis, (unsigned)st.nRows);
  ui::drawText(c, ui::fontCjk16(), 20, 84, titleBuf, ui::COL_TEXT, ui::COL_BG);

  const char* modeLbl = (s_viewMode == 0) ? "B·F·K 维度" : "审美 · 耗时";
  ui::aaFillRoundRect(c, BTN_MODE.x, BTN_MODE.y, BTN_MODE.w, BTN_MODE.h, 13,
                      ui::COL_CARD_HI, ui::COL_BORDER);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), BTN_MODE, modeLbl,
                             ui::COL_ACCENT, ui::COL_CARD_HI);

  if (s_hasScrolled) {
    char ageBuf[16];
    if (st.rankRxMs == 0) {
      snprintf(ageBuf, sizeof(ageBuf), "--");
    } else {
      uint32_t ageSec = (st.nowMs >= st.rankRxMs) ? ((st.nowMs - st.rankRxMs) / 1000u) : 0u;
      if (ageSec < 60) snprintf(ageBuf, sizeof(ageBuf), "%us前", (unsigned)ageSec);
      else if (ageSec < 3600) snprintf(ageBuf, sizeof(ageBuf), "%um前", (unsigned)(ageSec / 60));
      else snprintf(ageBuf, sizeof(ageBuf), "%uh前", (unsigned)(ageSec / 3600));
    }
    char footBuf[64];
    const char* src = st.rankSrc[0] ? st.rankSrc : "ModelDial";
    const char* lic = st.rankLic[0] ? st.rankLic : "CC BY 4.0";
    snprintf(footBuf, sizeof(footBuf), "%s · %s · 更新于 %s", src, lic, ageBuf);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 424, footBuf, ui::COL_MUTED, ui::COL_BG);
  } else {
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 424,
                         "上下滑动翻榜 · ModelDial CC BY 4.0", ui::COL_MUTED, ui::COL_BG);
  }
}

}  // namespace

void pageRankResetScroll() {
  s_scrollY = 0.0f;
  s_hasScrolled = false;
  s_flinging = false;
  s_bouncing = false;
  s_flingVel = 0.0f;
  s_sampleCount = 0;
  s_touchActive = false;
}

bool pageRankScrollPx(int16_t deltaY, uint8_t nRows) {
  s_lastNRows = nRows;
  s_flinging = false;
  s_bouncing = false;
  s_flingVel = 0.0f;
  float maxS = (float)maxScrollFor(nRows);
  float next = s_scrollY - (float)deltaY;
  if (next < 0.0f) next = 0.0f;
  if (next > maxS) next = maxS;
  if (next == s_scrollY) return false;
  s_scrollY = next;
  s_hasScrolled = true;
  return true;
}

void pageRankTouchDown(int16_t y, uint32_t ms) {
  s_flinging = false;
  s_bouncing = false;
  s_flingVel = 0.0f;
  s_touchActive = true;
  s_lastTouchY = y;
  s_sampleCount = 1;
  s_samples[0] = { y, ms };
}

bool pageRankTouchMove(int16_t y, uint32_t ms, uint8_t nRows) {
  s_lastNRows = nRows;
  if (nRows == 0) return false;

  int16_t dy = y - s_lastTouchY;
  s_lastTouchY = y;

  if (s_sampleCount == 0 ||
      ms != s_samples[s_sampleCount - 1].ms ||
      y != s_samples[s_sampleCount - 1].y) {
    if (s_sampleCount < 3) {
      s_samples[s_sampleCount++] = { y, ms };
    } else {
      s_samples[0] = s_samples[1];
      s_samples[1] = s_samples[2];
      s_samples[2] = { y, ms };
    }
  }

  float maxS = (float)maxScrollFor(nRows);
  float prev = s_scrollY;
  float next = prev;
  float remaining = -(float)dy;

  if (remaining != 0.0f) {
    // Keep 1:0.4 mapping symmetric while inside overscroll so reversing direction feels natural.
    if (next < 0.0f) {
      if (remaining > 0.0f) {
        float fingerToZero = -next / OVERSCROLL_DAMP;
        if (remaining <= fingerToZero) {
          next += remaining * OVERSCROLL_DAMP;
          remaining = 0.0f;
        } else {
          next = 0.0f;
          remaining -= fingerToZero;
        }
      } else {
        next += remaining * OVERSCROLL_DAMP;
        remaining = 0.0f;
      }
    } else if (next > maxS) {
      if (remaining < 0.0f) {
        float fingerToMax = (next - maxS) / OVERSCROLL_DAMP;
        if (-remaining <= fingerToMax) {
          next += remaining * OVERSCROLL_DAMP;
          remaining = 0.0f;
        } else {
          next = maxS;
          remaining += fingerToMax;
        }
      } else {
        next += remaining * OVERSCROLL_DAMP;
        remaining = 0.0f;
      }
    }

    if (remaining != 0.0f) {
      float cand = next + remaining;
      if (cand < 0.0f) {
        next = cand * OVERSCROLL_DAMP;
      } else if (cand > maxS) {
        next = maxS + (cand - maxS) * OVERSCROLL_DAMP;
      } else {
        next = cand;
      }
    }
  }

  if (next < -OVERSCROLL_MAX) next = -OVERSCROLL_MAX;
  if (next > maxS + OVERSCROLL_MAX) next = maxS + OVERSCROLL_MAX;

  if (fabsf(next - prev) < 0.01f) return false;
  s_scrollY = next;
  s_hasScrolled = true;
  return true;
}

void pageRankTouchUp(uint32_t ms, uint8_t nRows) {
  s_lastNRows = nRows;
  s_touchActive = false;

  if (nRows == 0) {
    s_flinging = false;
    s_bouncing = false;
    s_sampleCount = 0;
    return;
  }

  float maxS = (float)maxScrollFor(nRows);
  if (s_scrollY < 0.0f) {
    startBounce(0.0f, ms);
    s_sampleCount = 0;
    return;
  }
  if (s_scrollY > maxS) {
    startBounce(maxS, ms);
    s_sampleCount = 0;
    return;
  }

  if (s_sampleCount >= 2) {
    const TouchSample& first = s_samples[0];
    const TouchSample& last = s_samples[s_sampleCount - 1];
    int32_t staleMs = (int32_t)(ms - last.ms);
    int32_t dtMs = (int32_t)(last.ms - first.ms);
    if (staleMs <= 100 && dtMs > 0 && dtMs <= 200) {
      float v = ((float)(first.y - last.y) / (float)dtMs) * 16.67f;
      if (v > 80.0f) v = 80.0f;
      if (v < -80.0f) v = -80.0f;
      if (fabsf(v) >= FLING_STOP_V) {
        s_flinging = true;
        s_flingVel = v;
        s_lastTickMs = ms;
      }
    }
  }
  s_sampleCount = 0;
}

bool pageRankTick(uint32_t nowMs) {
  if (s_touchActive || s_lastNRows == 0) return false;

  float maxS = (float)maxScrollFor(s_lastNRows);

  if (s_bouncing) {
    int32_t elapsed = (int32_t)(nowMs - s_bounceStartMs);
    if (elapsed < 0) elapsed = 0;
    if ((uint32_t)elapsed >= BOUNCE_MS) {
      s_scrollY = s_bounceTo;
      s_bouncing = false;
      return true;
    }
    float u = (float)elapsed / (float)BOUNCE_MS;
    float inv = 1.0f - u;
    float ease = 1.0f - inv * inv * inv;  // easeOutCubic
    s_scrollY = s_bounceFrom + (s_bounceTo - s_bounceFrom) * ease;
    return true;
  }

  if (!s_flinging) return false;

  int32_t rawDt = (int32_t)(nowMs - s_lastTickMs);
  s_lastTickMs = nowMs;
  if (rawDt < 1) rawDt = 1;
  if (rawDt > 50) rawDt = 50;
  float frames = (float)rawDt / 16.67f;

  bool inOver = (s_scrollY < 0.0f || s_scrollY > maxS);
  if (!inOver) {
    s_scrollY += s_flingVel * frames;
    s_flingVel *= powf(FLING_DECAY, frames);

    if (s_scrollY < 0.0f) {
      s_scrollY *= OVERSCROLL_DAMP;
      if (s_scrollY <= -OVERSCROLL_MAX) {
        s_scrollY = -OVERSCROLL_MAX;
        startBounce(0.0f, nowMs);
        return true;
      }
    } else if (s_scrollY > maxS) {
      s_scrollY = maxS + (s_scrollY - maxS) * OVERSCROLL_DAMP;
      if (s_scrollY >= maxS + OVERSCROLL_MAX) {
        s_scrollY = maxS + OVERSCROLL_MAX;
        startBounce(maxS, nowMs);
        return true;
      }
    } else if (fabsf(s_flingVel) < FLING_STOP_V) {
      s_flinging = false;
      s_flingVel = 0.0f;
    }
  } else {
    // Decelerate quickly inside damped overscroll before snapping into easeOutCubic bounce.
    s_scrollY += s_flingVel * OVERSCROLL_DAMP * frames;
    s_flingVel *= powf(0.55f, frames);

    if (s_scrollY <= -OVERSCROLL_MAX) {
      s_scrollY = -OVERSCROLL_MAX;
      startBounce(0.0f, nowMs);
    } else if (s_scrollY >= maxS + OVERSCROLL_MAX) {
      s_scrollY = maxS + OVERSCROLL_MAX;
      startBounce(maxS, nowMs);
    } else if (fabsf(s_flingVel) < 1.0f ||
               (s_scrollY < 0.0f && s_flingVel > 0.0f) ||
               (s_scrollY > maxS && s_flingVel < 0.0f)) {
      startBounce(s_scrollY < 0.0f ? 0.0f : maxS, nowMs);
    }
  }
  return true;
}

bool pageRankIsFlinging() {
  return s_flinging || s_bouncing;
}

void pageRankDrawList(NovaCanvas& c, const NovaState& st) {
  s_lastNRows = st.nRows;
  if (st.nRows == 0) return;

  bool topTouched = drawRankRowsAndScrollbar(c, st);
  // Keep fb outside [96, 391) spotless so partial display pushes never leak clipped row edges.
  drawRankHeaderAndFooter(c, st, topTouched);
}

void pageRankDraw(NovaCanvas& c, const NovaState& st) {
  s_lastNRows = st.nRows;
  c.fillScreen(ui::COL_BG);

  int16_t maxS = maxScrollFor(st.nRows);
  if (!s_touchActive && !s_flinging && !s_bouncing) {
    if (s_scrollY > (float)maxS) s_scrollY = (float)maxS;
    if (s_scrollY < 0.0f) s_scrollY = 0.0f;
  }

  if (st.nRows == 0) {
    ui::drawTopBar(c, st, 3);
    ui::drawText(c, ui::fontCjk16(), 22, 85, "ModelDial 编程榜", ui::COL_TEXT, ui::COL_BG);
    ui::drawCard(c, 22, 120, 324, 144, ui::COL_CARD, ui::COL_BORDER, 18);
    ui::drawTextCentered(c, ui::fontCjk16(), UI_W / 2, 182, "暂无榜单数据",
                         ui::COL_TEXT, ui::COL_CARD);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 218, "等待 DSH 同步 ModelDial",
                         ui::COL_DIM, ui::COL_CARD);
    return;
  }

  drawRankRowsAndScrollbar(c, st);
  drawRankHeaderAndFooter(c, st, true);
}

UiAction pageRankTap(int16_t x, int16_t y) {
  if (BTN_MODE.contains(x, y)) {
    s_viewMode = (s_viewMode + 1) % 2;
    return UiAction{ UiAction::RANK_REDRAW, 0 };
  }
  return UiAction{};
}

bool pageRankHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings&,
                     ui::Rect* out) {
  if (!out || st.nRows == 0) return false;
  if (BTN_MODE.contains(x, y)) {
    *out = BTN_MODE;
    return true;
  }
  return false;
}
