#include "ui/pages.h"
#include "ui/theme.h"
#include <stdio.h>
#include <string.h>

namespace {

constexpr int16_t LIST_TOP    = 96;
constexpr int16_t LIST_BOTTOM = 394;
constexpr int16_t ROW_H       = 46;
constexpr int16_t ROW_PITCH   = 50;
constexpr ui::Rect BTN_MODE{ 246, 63, 102, 26 };

int16_t s_scrollY = 0;
uint8_t s_viewMode = 0;  // 0 = B/F/K bars, 1 = Aesthetic/Maint/Duration/Cost

uint16_t medalColor(uint8_t rank) {
  if (rank == 1) return ui::COL_GOLD;
  if (rank == 2) return ui::COL_SILVER;
  if (rank == 3) return ui::COL_BRONZE;
  return ui::COL_DIM;
}

int16_t maxScrollFor(uint8_t nRows) {
  int16_t viewH = LIST_BOTTOM - LIST_TOP;
  int16_t totalH = (int16_t)nRows * ROW_PITCH - 4;
  return (totalH > viewH) ? (totalH - viewH) : 0;
}

}  // namespace

void pageRankResetScroll() {
  s_scrollY = 0;
}

bool pageRankScrollPx(int16_t deltaY, uint8_t nRows) {
  int16_t maxS = maxScrollFor(nRows);
  int16_t next = s_scrollY - deltaY;
  if (next < 0) next = 0;
  if (next > maxS) next = maxS;
  if (next == s_scrollY) return false;
  s_scrollY = next;
  return true;
}

void pageRankDraw(NovaCanvas& c, const NovaState& st) {
  c.fillScreen(ui::COL_BG);

  int16_t maxS = maxScrollFor(st.nRows);
  if (s_scrollY > maxS) s_scrollY = maxS;

  if (st.nRows == 0) {
    ui::drawTopBar(c, st, 2);
    ui::drawText(c, ui::fontCjk16(), 24, 82, "ModelDial 编程榜", ui::COL_TEXT, ui::COL_BG);
    ui::drawCard(c, 24, 118, 320, 140, ui::COL_CARD, ui::COL_BORDER, 18);
    ui::drawTextCentered(c, ui::fontCjk16(), UI_W / 2, 180, "暂无榜单数据", ui::COL_TEXT, ui::COL_CARD);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 212, "等待 DSH 同步 ModelDial", ui::COL_DIM, ui::COL_CARD);
    return;
  }

  for (int i = 0; i < st.nRows && i < MAX_RANK_ROWS; i++) {
    const RankRow& row = st.rows[i];
    const int16_t rx = 20;
    const int16_t ry = LIST_TOP + i * ROW_PITCH - s_scrollY;
    const int16_t rw = 324;
    const int16_t rh = ROW_H;

    if (ry + rh <= LIST_TOP - 8 || ry >= LIST_BOTTOM + 8) continue;

    bool active = (st.activeModel[0] && strstr(st.activeModel, row.m) != nullptr);
    uint16_t rowBg = active ? rgb565(12, 34, 34) : ui::COL_CARD;
    uint16_t rowBd = active ? rgb565(36, 128, 104) : ui::COL_BORDER;
    ui::aaFillRoundRect(c, rx, ry, rw, rh, 12, rowBg, rowBd);

    // Rank pill (#1/#2/#3 gold/silver/bronze, others subtle pill)
    char rankStr[8];
    snprintf(rankStr, sizeof(rankStr), "%u", row.r);
    ui::Rect badgeR{ (int16_t)(rx + 8), (int16_t)(ry + 11), 24, 24 };
    if (row.r <= 3) {
      uint16_t mc = medalColor(row.r);
      ui::aaFillRoundRect(c, badgeR.x, badgeR.y, badgeR.w, badgeR.h, 7, mc, mc);
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), badgeR, rankStr, ui::COL_BG, mc);
    } else {
      ui::aaFillRoundRect(c, badgeR.x, badgeR.y, badgeR.w, badgeR.h, 7, ui::COL_CARD_HI, ui::COL_CARD_HI);
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), badgeR, rankStr, ui::COL_DIM, ui::COL_CARD_HI);
    }

    // Model name + optional NEW tag (top sub-row)
    int16_t nameMaxW = row.isNew ? 168 : 204;
    char modelBuf[36];
    ui::fitUtf8Line(c, ui::fontCjk14(), row.m, nameMaxW, modelBuf, sizeof(modelBuf));
    ui::drawText(c, ui::fontCjk14(), rx + 40, ry + 19, modelBuf,
                 active ? ui::COL_MINT : ui::COL_TEXT, rowBg);

    if (row.isNew) {
      ui::TextMetrics nm = ui::measureText(c, ui::fontCjk14(), modelBuf);
      int16_t nx = (int16_t)(rx + 46 + (int16_t)nm.w);
      if (nx < rx + 216) {
        ui::aaFillRoundRect(c, nx, ry + 5, 34, 16, 5, rgb565(18, 52, 46), rgb565(36, 128, 104));
        ui::drawTextCenteredInRect(c, ui::fontAscii14(), { nx, (int16_t)(ry + 5), 34, 16 },
                                   "NEW", ui::COL_MINT, rgb565(18, 52, 46));
      }
    }

    // Bottom sub-row: capability bars & metadata depending on mode
    if (s_viewMode == 0) {
      ui::drawText(c, ui::fontAscii14(), rx + 40, ry + 39, "B", ui::COL_ACCENT, rowBg);
      ui::drawProgressBar(c, rx + 51, ry + 32, 34, 5, row.b, ui::COL_ACCENT);

      ui::drawText(c, ui::fontAscii14(), rx + 93, ry + 39, "F", ui::COL_MINT, rowBg);
      ui::drawProgressBar(c, rx + 104, ry + 32, 34, 5, row.f, ui::COL_MINT);

      ui::drawText(c, ui::fontAscii14(), rx + 146, ry + 39, "K", ui::COL_VIOLET, rowBg);
      ui::drawProgressBar(c, rx + 157, ry + 32, 34, 5, row.k, ui::COL_VIOLET);

      if (row.e[0]) {
        ui::drawText(c, ui::fontCjk14(), rx + 200, ry + 39, row.e, ui::COL_MUTED, rowBg);
      }
    } else {
      ui::drawText(c, ui::fontAscii14(), rx + 40, ry + 39, "A", ui::COL_AMBER, rowBg);
      ui::drawProgressBar(c, rx + 51, ry + 32, 34, 5, row.a, ui::COL_AMBER);

      ui::drawText(c, ui::fontAscii14(), rx + 93, ry + 39, "Q", ui::COL_VIOLET, rowBg);
      ui::drawProgressBar(c, rx + 104, ry + 32, 34, 5, row.q, ui::COL_VIOLET);

      char meta[28];
      if (row.dur[0] && row.cost[0]) snprintf(meta, sizeof(meta), "%s  %s", row.dur, row.cost);
      else if (row.dur[0]) snprintf(meta, sizeof(meta), "%s", row.dur);
      else snprintf(meta, sizeof(meta), "%s", row.cost[0] ? row.cost : "--");
      ui::drawText(c, ui::fontCjk14(), rx + 148, ry + 39, meta, ui::COL_DIM, rowBg);
    }

    // Score (DIN 18px right-aligned)
    char scoreBuf[12];
    snprintf(scoreBuf, sizeof(scoreBuf), "%.1f", row.s);
    ui::drawTextRight(c, ui::fontAscii18(), rx + rw - 12, ry + 30, scoreBuf,
                      row.r == 1 ? ui::COL_GOLD : (active ? ui::COL_MINT : ui::COL_TEXT), rowBg);
  }

  // Mask overflow rows cleanly before drawing top header & bottom footer
  c.fillRect(0, 0, UI_W, LIST_TOP - 2, ui::COL_BG);
  c.fillRect(0, LIST_BOTTOM, UI_W, UI_H - LIST_BOTTOM, ui::COL_BG);
  ui::drawTopBar(c, st, 2);

  // Header title & live window range + mode toggle pill
  int firstVis = (s_scrollY + 12) / ROW_PITCH + 1;
  int lastVis = (s_scrollY + (LIST_BOTTOM - LIST_TOP) - 12) / ROW_PITCH + 1;
  if (lastVis > st.nRows) lastVis = st.nRows;
  char titleBuf[40];
  snprintf(titleBuf, sizeof(titleBuf), "编程榜 %d-%d/%u", firstVis, lastVis, (unsigned)st.nRows);
  ui::drawText(c, ui::fontCjk16(), 22, 81, titleBuf, ui::COL_TEXT, ui::COL_BG);

  const char* modeLbl = (s_viewMode == 0) ? "B·F·K 维度" : "审美 · 耗时";
  ui::aaFillRoundRect(c, BTN_MODE.x, BTN_MODE.y, BTN_MODE.w, BTN_MODE.h, 12, ui::COL_CARD_HI, ui::COL_BORDER);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), BTN_MODE, modeLbl, ui::COL_ACCENT, ui::COL_CARD_HI);

  // Right-edge scrollbar track & thumb
  if (maxS > 0) {
    const int16_t sbX = 351;
    const int16_t sbY = LIST_TOP;
    const int16_t sbH = LIST_BOTTOM - LIST_TOP - 4;
    ui::aaFillRoundRect(c, sbX, sbY, 4, sbH, 2, ui::COL_TRACK, ui::COL_TRACK);
    int16_t thumbH = (int16_t)((int32_t)sbH * (LIST_BOTTOM - LIST_TOP) / (st.nRows * ROW_PITCH));
    if (thumbH < 24) thumbH = 24;
    int16_t thumbY = sbY + (int16_t)((int32_t)(sbH - thumbH) * s_scrollY / maxS);
    ui::aaFillRoundRect(c, sbX, thumbY, 4, thumbH, 2, ui::COL_ACCENT, ui::COL_ACCENT);
  }

  ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 424, "上下滑动翻榜 · ModelDial CC BY 4.0", ui::COL_MUTED, ui::COL_BG);
}

UiAction pageRankTap(int16_t x, int16_t y) {
  if (BTN_MODE.contains(x, y)) {
    s_viewMode = (s_viewMode + 1) % 2;
    return UiAction{ UiAction::RANK_REDRAW, 0 };
  }
  return UiAction{};
}
