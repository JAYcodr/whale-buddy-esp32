#include "ui/pages.h"
#include "ui/theme.h"
#include <stdio.h>
#include <string.h>

namespace {
constexpr double kBalWarn = 10.0;
constexpr double kBalCrit = 2.0;

bool     s_syncPending = false;
uint32_t s_syncStartMs = 0;
uint32_t s_syncGen = 0;

uint16_t providerColor(const char* id) {
  if (strcmp(id, "ds") == 0) return ui::COL_ACCENT;
  if (strcmp(id, "oc") == 0) return ui::COL_MINT;
  if (strcmp(id, "od") == 0) return ui::COL_VIOLET;
  return ui::COL_ACCENT;
}

const char* formatBarLabel(const char* raw) {
  if (!raw || !raw[0]) return "--";
  if (strcmp(raw, "w") == 0)  return "周限";
  if (strcmp(raw, "m") == 0)  return "月限";
  if (strcmp(raw, "5h") == 0) return "5时";
  if (strcmp(raw, "d") == 0)  return "日限";
  return raw;
}

uint32_t windowTotalSec(const char* tag) {
  if (!tag || !tag[0]) return 0;
  if (strcmp(tag, "5h") == 0) return 18000u;
  if (strcmp(tag, "d") == 0)  return 86400u;
  if (strcmp(tag, "w") == 0)  return 604800u;
  if (strcmp(tag, "m") == 0)  return 2592000u;
  return 0;
}

uint16_t usageBarColor(uint8_t pct, uint16_t baseCol) {
  if (pct >= 90) return ui::COL_RED;
  if (pct >= 75) return ui::COL_AMBER;
  return baseCol;
}

void formatAge(uint32_t ageSec, char* out, size_t cap) {
  if (ageSec < 60) snprintf(out, cap, "%us前", (unsigned)ageSec);
  else if (ageSec < 3600) snprintf(out, cap, "%um前", (unsigned)(ageSec / 60));
  else snprintf(out, cap, "%uh前", (unsigned)(ageSec / 3600));
}

int16_t cardY(int i, int nShow) {
  if (nShow >= 3) {
    return (int16_t)(66 + i * 122);
  }
  // Distribute 1 or 2 cards symmetrically across y in [66, 422).
  const int16_t gap = (int16_t)((356 - nShow * 112) / (nShow + 1));
  return (int16_t)(66 + gap + i * (112 + gap));
}
}  // namespace

void pageQuotaDraw(NovaCanvas& c, const NovaState& st) {
  c.fillScreen(ui::COL_BG);
  ui::drawTopBar(c, st, 1);

  if (s_syncPending &&
      (st.quotaGen != s_syncGen || st.nowMs < s_syncStartMs ||
       (st.nowMs - s_syncStartMs) >= 3000u)) {
    s_syncPending = false;
  }

  if (st.nCards == 0) {
    ui::drawCard(c, 24, 118, 320, 140, ui::COL_CARD, ui::COL_BORDER, 18);
    ui::drawTextCentered(c, ui::fontCjk16(), UI_W / 2, 178,
                         "暂未收到订阅数据", ui::COL_TEXT, ui::COL_CARD);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 212,
                         "请确认电脑端 DSH 插件已运行", ui::COL_DIM, ui::COL_CARD);
    return;
  }

  const uint32_t localElapsed = (st.quotaRxMs > 0 && st.nowMs > st.quotaRxMs)
                                  ? ((st.nowMs - st.quotaRxMs) / 1000u)
                                  : 0u;
  const int nShow = st.nCards > 3 ? 3 : st.nCards;

  for (int i = 0; i < nShow; i++) {
    const QuotaCard& card = st.cards[i];
    const int16_t cx = 20;
    const int16_t cy = cardY(i, nShow);
    const int16_t cw = 328;
    const int16_t ch = 112;
    const uint16_t accent = providerColor(card.id);

    ui::drawCard(c, cx, cy, cw, ch, ui::COL_CARD, ui::COL_BORDER, 18);
    ui::aaFillRoundRect(c, cx + 10, cy + 16, 4, ch - 32, 2, accent, accent);

    // Header: Provider name + sync feedback or live incrementing age badge
    ui::drawText(c, ui::fontCjk16(), cx + 22, cy + 31, card.name, ui::COL_TEXT, ui::COL_CARD);
    if (s_syncPending) {
      ui::drawTextRight(c, ui::fontCjk14(), cx + cw - 14, cy + 30,
                        "同步中…", ui::COL_ACCENT, ui::COL_CARD);
    } else {
      char ageBuf[16];
      uint32_t liveAge = card.ageSec + localElapsed;
      formatAge(liveAge, ageBuf, sizeof(ageBuf));
      ui::drawTextRight(c, ui::fontCjk14(), cx + cw - 14, cy + 30, ageBuf,
                        (liveAge >= 600) ? ui::COL_AMBER : ui::COL_MUTED, ui::COL_CARD);
    }

    if (strcmp(card.kind, "bal") == 0) {
      char valBuf[24];
      snprintf(valBuf, sizeof(valBuf), "%.2f", card.val);
      ui::TextMetrics vm = ui::measureText(c, ui::fontNum24(), valBuf);
      const uint16_t balCol = (card.val < kBalCrit)
                                ? ui::COL_RED
                                : ((card.val < kBalWarn) ? ui::COL_AMBER : ui::COL_MINT);
      ui::drawText(c, ui::fontNum24(), cx + 22, cy + 75, valBuf, balCol, ui::COL_CARD);

      const char* cur = card.cur[0] ? card.cur : "CNY";
      int16_t tagX = (int16_t)(cx + 22 + (int16_t)vm.w + 12);
      int16_t tagY = (int16_t)(cy + 52);
      ui::aaFillRoundRect(c, tagX, tagY, 48, 24, 7, ui::COL_CARD_HI, ui::COL_CARD_HI);
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), { tagX, tagY, 48, 24 },
                                 cur, ui::COL_DIM, ui::COL_CARD_HI);
      ui::drawText(c, ui::fontCjk14(), cx + 22, cy + 101,
                   "可用账户余额", ui::COL_DIM, ui::COL_CARD);
    } else if (strcmp(card.kind, "win") == 0) {
      int bars = card.nBars > 2 ? 2 : card.nBars;
      for (int b = 0; b < bars; b++) {
        const int16_t rowY = cy + 60 + b * 31;
        const QuotaBar& qb = card.bars[b];
        ui::drawText(c, ui::fontCjk14(), cx + 22, rowY + 4,
                     formatBarLabel(qb.tag), ui::COL_DIM, ui::COL_CARD);

        const int16_t barX = cx + 64;
        const int16_t barY = rowY - 6;
        // 146px bar + pct right-aligned at cx + 254 leaves >= 6px before 6-char reset strings.
        const int16_t barW = 146;
        const int16_t barH = 9;
        const uint16_t barCol = usageBarColor(qb.pct, accent);
        ui::drawProgressBar(c, barX, barY, barW, barH, qb.pct, barCol);

        const uint32_t liveReset = (qb.resetSec > localElapsed)
                                     ? (qb.resetSec - localElapsed)
                                     : 0u;
        const uint32_t totalSec = windowTotalSec(qb.tag);
        float pace = 0.0f;
        if (totalSec > 0) {
          pace = (liveReset < totalSec)
                   ? (float)(totalSec - liveReset) / (float)totalSec
                   : 0.0f;
          if (pace < 0.0f) pace = 0.0f;
          if (pace > 1.0f) pace = 1.0f;
          int16_t tickX = (int16_t)(barX + (int16_t)(barW * pace + 0.5f));
          if (tickX < barX) tickX = barX;
          if (tickX > barX + barW - 2) tickX = barX + barW - 2;
          // Draw pace tick 2px above and below the 9px bar so pace is visible against track.
          c.fillRect(tickX, barY - 2, 2, 13, ui::COL_TEXT);
        }

        uint16_t pctCol = accent;
        if (qb.pct == 0) {
          pctCol = ui::COL_MUTED;
        } else if (qb.pct >= 90) {
          pctCol = ui::COL_RED;
        } else if (qb.pct >= 75 || (totalSec > 0 && (float)qb.pct > pace * 100.0f + 10.0f)) {
          pctCol = ui::COL_AMBER;
        }

        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%u%%", qb.pct);
        ui::drawTextRight(c, ui::fontAscii14(), cx + 254, rowY + 4, pctBuf, pctCol, ui::COL_CARD);

        char rstBuf[16];
        ui::fmtResetTime(liveReset, rstBuf, sizeof(rstBuf));
        ui::drawTextRight(c, ui::fontCjk14(), cx + cw - 14, rowY + 4,
                          rstBuf, ui::COL_MUTED, ui::COL_CARD);
      }
    } else {
      int16_t bx = (int16_t)(cx + 22);
      int16_t by = (int16_t)(cy + 54);
      ui::aaFillRoundRect(c, bx, by, 74, 26, 8, rgb565(42, 28, 12), rgb565(42, 28, 12));
      ui::drawTextCenteredInRect(c, ui::fontCjk14(), { bx, by, 74, 26 },
                                 "未配置", ui::COL_AMBER, rgb565(42, 28, 12));
      char whyBuf[64];
      ui::fitUtf8Line(c, ui::fontCjk14(), card.why[0] ? card.why : "等待凭据",
                      cw - 120, whyBuf, sizeof(whyBuf));
      ui::drawText(c, ui::fontCjk14(), cx + 106, cy + 72, whyBuf, ui::COL_DIM, ui::COL_CARD);
    }
  }
}

UiAction pageQuotaTap(int16_t, int16_t y, const NovaState& st) {
  if (y >= 60) {
    s_syncPending = true;
    s_syncStartMs = st.nowMs;
    s_syncGen = st.quotaGen;
    return UiAction{ UiAction::SYNC_HOST, 0 };
  }
  return UiAction{};
}

bool pageQuotaTick(uint32_t nowMs) {
  if (s_syncPending && (nowMs < s_syncStartMs || (nowMs - s_syncStartMs) >= 3000u)) {
    s_syncPending = false;
    return true;
  }
  return false;
}

bool pageQuotaHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings&,
                      ui::Rect* out) {
  if (!out) return false;
  const int nShow = st.nCards > 3 ? 3 : st.nCards;
  for (int i = 0; i < nShow; i++) {
    ui::Rect r{ 20, cardY(i, nShow), 328, 112 };
    if (r.contains(x, y)) {
      *out = r;
      return true;
    }
  }
  return false;
}
