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

// 宿主的 why 是机器可读原因码（host-plugin/lib/fetchers.js），不能原样上屏。
// 映射后每条控制在 6 汉字内：state.h 里 why 只有 char[24]，且卡片宽度有限。
struct WhyInfo {
  const char* label;   // 胶囊里的短标签
  const char* detail;  // 右侧一句说明
  uint16_t    fg;
  uint16_t    bg;
};

WhyInfo whyText(const char* why) {
  const uint16_t dimBg = rgb565(18, 28, 40);
  const uint16_t ambBg = rgb565(42, 28, 12);
  const uint16_t redBg = rgb565(46, 14, 22);
  if (!why || !why[0]) {
    return { "未配置", "等待凭据", ui::COL_AMBER, ambBg };
  }
  if (strcmp(why, "no-session") == 0) {
    // 没登录 / 没连上 —— 不是「配置错了」，不该用琥珀警告色。
    return { "未连接", "登录后自动同步", ui::COL_MUTED, dimBg };
  }
  if (strcmp(why, "no-key") == 0) {
    return { "未配置", "填入密钥后同步", ui::COL_AMBER, ambBg };
  }
  if (strcmp(why, "reauth") == 0) {
    return { "需授权", "重新登录后同步", ui::COL_AMBER, ambBg };
  }
  if (strcmp(why, "fetch-err") == 0) {
    return { "同步失败", "点本卡重试", ui::COL_RED, redBg };
  }
  return { "未配置", "暂不可用", ui::COL_MUTED, dimBg };
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
  ui::drawTopBar(c, st, 2);

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
    char nameBuf[32];
    ui::fitUtf8Line(c, ui::fontCjk16(), card.name, cw - 96, nameBuf, sizeof(nameBuf));
    ui::drawText(c, ui::fontCjk16(), cx + 22, cy + 31, nameBuf, ui::COL_TEXT, ui::COL_CARD);
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
        // 152px bar + pct right-aligned at cx + 254 leaves clean breathing room before reset time.
        const int16_t barW = 152;
        const int16_t barH = 9;
        const uint16_t barCol = usageBarColor(qb.pct, accent);
        ui::drawProgressBar(c, barX, barY, barW, barH, qb.pct, barCol);

        const uint32_t liveReset = (qb.resetSec > localElapsed)
                                     ? (qb.resetSec - localElapsed)
                                     : 0u;
        const uint32_t totalSec = windowTotalSec(qb.tag);
        if (totalSec > 0 && liveReset > 0 && liveReset < totalSec) {
          float pace = (float)(totalSec - liveReset) / (float)totalSec;
          if (pace < 0.0f) pace = 0.0f;
          if (pace > 1.0f) pace = 1.0f;
          int16_t tickX = (int16_t)(barX + (int16_t)(barW * pace + 0.5f));
          if (tickX < barX + 2) tickX = barX + 2;
          if (tickX > barX + barW - 4) tickX = barX + barW - 4;
          int16_t fillW = (int16_t)((int32_t)barW * (qb.pct > 100 ? 100 : qb.pct) / 100);
          if (fillW > 0 && fillW < barH) fillW = barH;
          // Flush gauge notch inside the bar height so it never juts out like a stray artifact.
          uint16_t tickCol = (tickX < barX + fillW)
                               ? ui::blend565(barCol, ui::COL_CARD, 185)
                               : ui::COL_DIM;
          c.fillRect(tickX, barY + 1, 2, barH - 2, tickCol);
        }

        // Keep percentage color consistent with the progress bar (muted only when 0%).
        uint16_t pctCol = (qb.pct == 0) ? ui::COL_MUTED : barCol;

        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%u%%", qb.pct);
        ui::drawTextRight(c, ui::fontAscii14(), cx + 254, rowY + 4, pctBuf, pctCol, ui::COL_CARD);

        char rstBuf[16];
        ui::fmtResetTime(liveReset, rstBuf, sizeof(rstBuf));
        ui::drawTextRight(c, ui::fontCjk14(), cx + cw - 14, rowY + 4,
                          rstBuf, ui::COL_MUTED, ui::COL_CARD);
      }
    } else {
      // 空态：不把宿主原因码原样上屏，走 whyText() 映射。
      const WhyInfo info = whyText(card.why);
      const int16_t bx = (int16_t)(cx + 22);
      const int16_t by = (int16_t)(cy + 54);
      const int16_t bh = 26;

      // 胶囊宽度跟标签走，避免「未连接」这类 3 字标签撑不满 74px 的空洞，
      // 也避免将来更长的标签被硬裁。
      ui::TextMetrics lm = ui::measureText(c, ui::fontCjk14(), info.label);
      int16_t pw = (int16_t)(lm.w + 20);
      if (pw < 74) pw = 74;
      const int16_t maxPw = (int16_t)(cw - 120);
      if (pw > maxPw) pw = maxPw;

      ui::aaFillRoundRect(c, bx, by, pw, bh, 8, info.bg, info.bg);
      ui::drawTextCenteredInRect(c, ui::fontCjk14(), { bx, by, pw, bh },
                                 info.label, info.fg, info.bg);

      char whyBuf[64];
      const int16_t detailX = (int16_t)(bx + pw + 10);
      const int16_t detailW = (int16_t)(cx + cw - 14 - detailX);
      ui::fitUtf8Line(c, ui::fontCjk14(), info.detail, detailW, whyBuf, sizeof(whyBuf));
      ui::drawText(c, ui::fontCjk14(), detailX, (int16_t)(cy + 72), whyBuf,
                   ui::COL_DIM, ui::COL_CARD);
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
