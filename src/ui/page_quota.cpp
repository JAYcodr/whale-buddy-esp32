#include "ui/pages.h"
#include "ui/theme.h"
#include <stdio.h>
#include <string.h>

namespace {
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
}  // namespace

void pageQuotaDraw(NovaCanvas& c, const NovaState& st) {
  c.fillScreen(ui::COL_BG);
  ui::drawTopBar(c, st, 1);

  if (st.nCards == 0) {
    ui::drawCard(c, 24, 118, 320, 140, ui::COL_CARD, ui::COL_BORDER, 18);
    ui::drawTextCentered(c, ui::fontCjk16(), UI_W / 2, 178, "暂未收到订阅数据", ui::COL_TEXT, ui::COL_CARD);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 212, "请确认电脑端 DSH 插件已运行", ui::COL_DIM, ui::COL_CARD);
    return;
  }

  const uint32_t localElapsed = (st.quotaRxMs > 0 && st.nowMs > st.quotaRxMs)
                                  ? ((st.nowMs - st.quotaRxMs) / 1000u)
                                  : 0u;

  for (int i = 0; i < st.nCards && i < 3; i++) {
    const QuotaCard& card = st.cards[i];
    const int16_t cx = 20;
    const int16_t cy = 66 + i * 122;
    const int16_t cw = 328;
    const int16_t ch = 112;
    const uint16_t accent = providerColor(card.id);

    ui::drawCard(c, cx, cy, cw, ch, ui::COL_CARD, ui::COL_BORDER, 18);
    ui::aaFillRoundRect(c, cx + 10, cy + 16, 4, ch - 32, 2, accent, accent);

    // Header: Provider name + live incrementing age badge
    ui::drawText(c, ui::fontCjk16(), cx + 22, cy + 31, card.name, ui::COL_TEXT, ui::COL_CARD);
    char ageBuf[16];
    uint32_t liveAge = card.ageSec + localElapsed;
    formatAge(liveAge, ageBuf, sizeof(ageBuf));
    ui::drawTextRight(c, ui::fontCjk14(), cx + cw - 14, cy + 30, ageBuf,
                      (liveAge >= 600) ? ui::COL_AMBER : ui::COL_MUTED, ui::COL_CARD);

    if (strcmp(card.kind, "bal") == 0) {
      char valBuf[24];
      snprintf(valBuf, sizeof(valBuf), "%.2f", card.val);
      ui::TextMetrics vm = ui::measureText(c, ui::fontNum24(), valBuf);
      ui::drawText(c, ui::fontNum24(), cx + 22, cy + 75, valBuf, ui::COL_MINT, ui::COL_CARD);

      const char* cur = card.cur[0] ? card.cur : "CNY";
      int16_t tagX = (int16_t)(cx + 22 + (int16_t)vm.w + 12);
      int16_t tagY = (int16_t)(cy + 52);
      ui::aaFillRoundRect(c, tagX, tagY, 48, 24, 7, ui::COL_CARD_HI, ui::COL_CARD_HI);
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), { tagX, tagY, 48, 24 }, cur, ui::COL_DIM, ui::COL_CARD_HI);
      ui::drawText(c, ui::fontCjk14(), cx + 22, cy + 101, "可用账户余额", ui::COL_DIM, ui::COL_CARD);
    } else if (strcmp(card.kind, "win") == 0) {
      int bars = card.nBars > 2 ? 2 : card.nBars;
      for (int b = 0; b < bars; b++) {
        const int16_t rowY = cy + 60 + b * 31;
        const QuotaBar& qb = card.bars[b];
        ui::drawText(c, ui::fontCjk14(), cx + 22, rowY + 4, formatBarLabel(qb.tag), ui::COL_DIM, ui::COL_CARD);

        uint16_t barCol = usageBarColor(qb.pct, accent);
        ui::drawProgressBar(c, cx + 68, rowY - 6, 118, 9, qb.pct, barCol);

        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%u%%", qb.pct);
        ui::drawTextRight(c, ui::fontAscii14(), cx + 240, rowY + 4, pctBuf, barCol, ui::COL_CARD);

        char rstBuf[16];
        uint32_t liveReset = (qb.resetSec > localElapsed) ? (qb.resetSec - localElapsed) : 0u;
        ui::fmtResetTime(liveReset, rstBuf, sizeof(rstBuf));
        ui::drawTextRight(c, ui::fontCjk14(), cx + cw - 14, rowY + 4, rstBuf, ui::COL_MUTED, ui::COL_CARD);
      }
    } else {
      int16_t bx = (int16_t)(cx + 22);
      int16_t by = (int16_t)(cy + 54);
      ui::aaFillRoundRect(c, bx, by, 74, 26, 8, rgb565(42, 28, 12), rgb565(42, 28, 12));
      ui::drawTextCenteredInRect(c, ui::fontCjk14(), { bx, by, 74, 26 }, "未配置", ui::COL_AMBER, rgb565(42, 28, 12));
      char whyBuf[64];
      ui::fitUtf8Line(c, ui::fontCjk14(), card.why[0] ? card.why : "等待凭据", cw - 120, whyBuf, sizeof(whyBuf));
      ui::drawText(c, ui::fontCjk14(), cx + 106, cy + 72, whyBuf, ui::COL_DIM, ui::COL_CARD);
    }
  }
}

UiAction pageQuotaTap(int16_t, int16_t y) {
  if (y >= 60) {
    return UiAction{ UiAction::SYNC_HOST, 0 };
  }
  return UiAction{};
}
