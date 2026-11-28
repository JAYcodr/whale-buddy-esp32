#include "ui/pages.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/tide_palette.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int16_t HDR_TOP_BASE     = 62;
constexpr int16_t VIEW_TOP_BASE    = 108;
constexpr int16_t VIEW_BOT_BASE    = 374;
constexpr int16_t DOCK_BTN_Y_BASE  = UI_H - 64;  // 384..432 = UI_SAFE_B
constexpr int16_t DOCK_BTN_H       = 48;

constexpr int16_t CARD_PAD_X       = 12;
constexpr int16_t CARD_GAP         = 6;
constexpr int16_t COMPACT_CARD_H   = 62;
constexpr int16_t MINI_CARD_H      = 38;
constexpr int16_t LAST_SAY_H       = 56;
constexpr int16_t DELIV_SESS_H     = 62;
constexpr int16_t DELIV_FOLD_H     = 22;
constexpr int16_t DELIV_CARD_H     = 164;

constexpr int16_t DRAFT_CARD_Y     = 114;
constexpr int16_t DRAFT_CARD_H     = 248;

constexpr uint16_t COL_ACTIVE_BG   = rgb565(26, 42, 68);
constexpr uint16_t COL_DRAFT_BG    = rgb565(16, 28, 46);
constexpr uint16_t COL_HOLD_BG     = rgb565(8, 28, 44);
constexpr uint16_t COL_WAIT_BG     = rgb565(24, 20, 10);
constexpr uint16_t COL_ERR_BG      = rgb565(46, 14, 22);
constexpr uint16_t COL_SENT_IC_BG  = rgb565(16, 44, 62);
constexpr uint16_t COL_RUN_IC_BG   = rgb565(48, 38, 12);
constexpr uint16_t COL_MUTE_IC_BG  = rgb565(24, 32, 46);

uint8_t boardCount(const NovaState& st) {
  return (st.nBoardSessions <= MAX_BOARD_SESSIONS)
           ? st.nBoardSessions
           : (uint8_t)MAX_BOARD_SESSIONS;
}

uint16_t sessDotColor(BoardSessState s) {
  switch (s) {
    case BoardSessState::RUN:  return ui::COL_ACCENT;
    case BoardSessState::WAIT: return ui::COL_AMBER;
    case BoardSessState::ERR:  return ui::COL_RED;
    case BoardSessState::IDLE:
    default:                   return ui::COL_MUTED;
  }
}

void fmtSessTime(const BoardSession& s, uint32_t nowMs, uint32_t boardRxMs,
                 char* out, size_t cap) {
  if (s.state == BoardSessState::RUN) {
    uint32_t extra = (boardRxMs > 0 && nowMs >= boardRxMs) ? ((nowMs - boardRxMs) / 1000u) : 0u;
    uint32_t sec = s.elapsedSec + extra;
    uint32_t m = sec / 60u;
    uint32_t r = sec % 60u;
    snprintf(out, cap, "跑着 %lu:%02lu", (unsigned long)m, (unsigned long)r);
  } else if (s.state == BoardSessState::WAIT) {
    snprintf(out, cap, "等审批");
  } else if (s.state == BoardSessState::ERR) {
    snprintf(out, cap, "异常");
  } else {
    snprintf(out, cap, "停着");
  }
}

void fmtAgoText(uint32_t baseSec, uint32_t rxMs, uint32_t nowMs, char* out, size_t cap) {
  uint32_t extra = (rxMs > 0 && nowMs >= rxMs) ? ((nowMs - rxMs) / 1000u) : 0u;
  uint32_t sec = baseSec + extra;
  if (sec < 10u) {
    snprintf(out, cap, "刚刚");
  } else if (sec < 60u) {
    snprintf(out, cap, "%lu 秒前", (unsigned long)sec);
  } else {
    snprintf(out, cap, "%lu 分钟前", (unsigned long)(sec / 60u));
  }
}

int expandedIndex(const NovaState& st) {
  if (!st.boardExpandedId[0]) return -1;
  const uint8_t n = boardCount(st);
  for (uint8_t i = 0; i < n; i++) {
    if (strcmp(st.boardSessions[i].id, st.boardExpandedId) == 0) return (int)i;
  }
  return -1;
}

int16_t expandedCardHeight(const BoardSession& s) {
  uint8_t steps = (s.nSteps <= MAX_SESS_STEPS) ? s.nSteps : (uint8_t)MAX_SESS_STEPS;
  if (steps == 0) return 86;
  return (int16_t)(86 + 10 + (int16_t)steps * 20 + 6);
}

int deliverySessionIndex(const NovaState& st) {
  const uint8_t n = boardCount(st);
  if (n == 0) return -1;
  const char* id = st.chatDelivery.targetId[0] ? st.chatDelivery.targetId : st.boardTargetId;
  if (id && id[0]) {
    for (uint8_t i = 0; i < n; i++) {
      if (strcmp(st.boardSessions[i].id, id) == 0) return (int)i;
    }
  }
  return 0;
}

ui::Rect dockBtnRect(const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  return ui::Rect{
    (int16_t)(UI_SAFE_L + ox),
    (int16_t)(DOCK_BTN_Y_BASE + oy),
    (int16_t)UI_SAFE_W,
    DOCK_BTN_H,
  };
}

ui::Rect headerBadgeRect(const NovaState& st, const char* txt) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  int16_t w = 52;
  if (txt && txt[0]) {
    size_t len = strlen(txt);
    w = (int16_t)(len * 8 + 20);
    if (w < 50) w = 50;
    if (w > 110) w = 110;
  }
  return ui::Rect{
    (int16_t)(UI_SAFE_R + ox - w),
    (int16_t)(HDR_TOP_BASE + 5 + oy),
    w,
    26,
  };
}

void computeHeaderBadge(const NovaState& st, char* outTxt, size_t cap,
                        uint16_t* outBg, uint16_t* outBd, uint16_t* outFg,
                        bool* outHasWait) {
  const uint8_t n = boardCount(st);
  uint8_t waits = 0;
  uint8_t runs = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (st.boardSessions[i].state == BoardSessState::WAIT) waits++;
    else if (st.boardSessions[i].state == BoardSessState::RUN) runs++;
  }
  if (outHasWait) *outHasWait = (waits > 0);
  if (waits > 0) {
    snprintf(outTxt, cap, "%u 项待批", (unsigned)waits);
    *outBg = COL_WAIT_BG;
    *outBd = ui::COL_AMBER;
    *outFg = ui::COL_AMBER;
  } else if (runs > 0) {
    snprintf(outTxt, cap, "%u 个在跑", (unsigned)runs);
    *outBg = ui::COL_CARD;
    *outBd = ui::COL_BORDER;
    *outFg = ui::COL_ACCENT;
  } else {
    snprintf(outTxt, cap, "空闲");
    *outBg = ui::COL_CARD;
    *outBd = ui::COL_BORDER;
    *outFg = ui::COL_DIM;
  }
}

ui::Rect boardCardRect(const NovaState& st, uint8_t idx, uint8_t showCount, int expIdx) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  int16_t y = (int16_t)(VIEW_TOP_BASE + oy);
  for (uint8_t i = 0; i < showCount; i++) {
    int16_t h = COMPACT_CARD_H;
    if (expIdx >= 0) {
      h = ((int)i == expIdx) ? expandedCardHeight(st.boardSessions[i]) : MINI_CARD_H;
    }
    if (i == idx) {
      return ui::Rect{ (int16_t)(UI_SAFE_L + ox), y, (int16_t)UI_SAFE_W, h };
    }
    y = (int16_t)(y + h + CARD_GAP);
  }
  return ui::Rect{ 0, 0, 0, 0 };
}

ui::Rect lastSayRect(const NovaState& st, uint8_t showCount) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  int16_t y = (int16_t)(VIEW_TOP_BASE + oy);
  if (showCount > 0) {
    y = (int16_t)(y + showCount * COMPACT_CARD_H + (showCount - 1) * CARD_GAP + 8);
  }
  if (y + LAST_SAY_H > VIEW_BOT_BASE + oy) {
    y = (int16_t)(VIEW_BOT_BASE + oy - LAST_SAY_H);
  }
  return ui::Rect{ (int16_t)(UI_SAFE_L + ox), y, (int16_t)UI_SAFE_W, LAST_SAY_H };
}

ui::Rect delivTargetRect(const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  return ui::Rect{
    (int16_t)(UI_SAFE_L + ox),
    (int16_t)(VIEW_TOP_BASE + oy),
    (int16_t)UI_SAFE_W,
    DELIV_SESS_H,
  };
}

ui::Rect delivFoldRect(const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  return ui::Rect{
    (int16_t)(UI_SAFE_L + ox),
    (int16_t)(VIEW_TOP_BASE + DELIV_SESS_H + 4 + oy),
    (int16_t)UI_SAFE_W,
    DELIV_FOLD_H,
  };
}

ui::Rect delivCardRect(const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const bool hasSess = (boardCount(st) > 0);
  const bool hasFold = (boardCount(st) > 1);
  int16_t y = (int16_t)(VIEW_TOP_BASE + oy);
  if (hasSess) y = (int16_t)(y + DELIV_SESS_H + 4);
  if (hasFold) y = (int16_t)(y + DELIV_FOLD_H + 4);
  int16_t h = (int16_t)(VIEW_BOT_BASE + oy - y - 4);
  if (h > DELIV_CARD_H) h = DELIV_CARD_H;
  if (h < 120) h = 120;
  return ui::Rect{ (int16_t)(UI_SAFE_L + ox), y, (int16_t)UI_SAFE_W, h };
}

ui::Rect draftCardRect(const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  return ui::Rect{
    (int16_t)(UI_SAFE_L + ox),
    (int16_t)(DRAFT_CARD_Y + oy),
    (int16_t)UI_SAFE_W,
    DRAFT_CARD_H,
  };
}

ui::Rect draftTargetRect(const NovaState& st) {
  const ui::Rect dc = draftCardRect(st);
  return ui::Rect{ (int16_t)(dc.x + 14), (int16_t)(dc.y + 122), (int16_t)(dc.w - 28), 36 };
}

void draftActionRects(const NovaState& st, const BoardSession* target,
                      ui::Rect* outCancel, ui::Rect* outPrimary, ui::Rect* outSteer) {
  const ui::Rect dc = draftCardRect(st);
  const int16_t bx = (int16_t)(dc.x + 14);
  const int16_t bw = (int16_t)(dc.w - 28);
  const int16_t by = (int16_t)(dc.y + 170);
  const int16_t bh = 40;
  if (target && target->state == BoardSessState::RUN) {
    const int16_t gap = 8;
    const int16_t w3 = (int16_t)((bw - 2 * gap) / 3);
    if (outCancel)  *outCancel  = ui::Rect{ bx, by, w3, bh };
    if (outPrimary) *outPrimary = ui::Rect{ (int16_t)(bx + w3 + gap), by, w3, bh };
    if (outSteer)   *outSteer   = ui::Rect{ (int16_t)(bx + 2 * (w3 + gap)), by, (int16_t)(bw - 2 * (w3 + gap)), bh };
  } else {
    const int16_t gap = 10;
    const int16_t w2 = (int16_t)((bw - gap) / 2);
    if (outCancel)  *outCancel  = ui::Rect{ bx, by, w2, bh };
    if (outPrimary) *outPrimary = ui::Rect{ (int16_t)(bx + w2 + gap), by, (int16_t)(bw - w2 - gap), bh };
    if (outSteer)   *outSteer   = ui::Rect{ 0, 0, 0, 0 };
  }
}

void drawHeader(NovaCanvas& c, const NovaState& st, const BoardSession* target) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);

  c.fillRect(0, 60, UI_W, (int16_t)(VIEW_TOP_BASE + oy - 60), ui::COL_BG);
  ui::drawTopBar(c, st, 5);

  ui::drawText(c, ui::fontCjk16(), (int16_t)(UI_SAFE_L + 2 + ox), (int16_t)(81 + oy),
               "命令方块模式", ui::COL_TEXT, ui::COL_BG);

  char sub[72] = {};
  uint16_t subCol = ui::COL_DIM;
  if (st.chatDraftActive) {
    snprintf(sub, sizeof(sub), "说完了 · 确认一下");
  } else if (target) {
    char rawSub[72];
    snprintf(rawSub, sizeof(rawSub), "发往 · %s", target->name);
    ui::fitUtf8Line(c, ui::fontCjk14(), rawSub, (int16_t)(UI_SAFE_W - 114), sub, sizeof(sub));
    subCol = ui::COL_ACCENT;
  } else {
    snprintf(sub, sizeof(sub), "全局 · 只读巡检");
  }
  ui::drawText(c, ui::fontCjk14(), (int16_t)(UI_SAFE_L + 2 + ox), (int16_t)(100 + oy),
               sub, subCol, ui::COL_BG);

  char badgeTxt[24] = {};
  uint16_t bg = ui::COL_CARD, bd = ui::COL_BORDER, fg = ui::COL_DIM;
  bool hasWait = false;
  computeHeaderBadge(st, badgeTxt, sizeof(badgeTxt), &bg, &bd, &fg, &hasWait);
  (void)hasWait;
  const ui::Rect br = headerBadgeRect(st, badgeTxt);
  ui::aaFillRoundRect(c, br.x, br.y, br.w, br.h, 13, bg, bd);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), br, badgeTxt, fg, bg);
}

void drawSessionCard(NovaCanvas& c, const NovaState& st, const BoardSession& s,
                     ui::Rect r, bool selected, bool expanded, bool mini) {
  const bool waiting = (s.state == BoardSessState::WAIT);
  const uint16_t bg = selected ? COL_ACTIVE_BG : ui::COL_CARD;
  const uint16_t bd = selected ? ui::COL_ACCENT
                               : (waiting ? ui::COL_AMBER : ui::COL_BORDER);
  ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 14, bg, bd);

  if (selected) {
    c.fillRect((int16_t)(r.x + 1), (int16_t)(r.y + 10), 4, (int16_t)(r.h - 20), ui::COL_ACCENT);
  } else if (waiting) {
    c.fillRect((int16_t)(r.x + 1), (int16_t)(r.y + 10), 3, (int16_t)(r.h - 20), ui::COL_AMBER);
  }

  char timeBuf[24];
  fmtSessTime(s, st.nowMs, st.boardRxMs, timeBuf, sizeof(timeBuf));
  const uint16_t timeW = ui::measureText(c, ui::fontCjk14(), timeBuf).w;
  const int16_t rightReserve = (int16_t)(timeW + (selected ? 34 : 14));

  const int16_t row1BaseY = mini ? (int16_t)(r.y + 24) : (int16_t)(r.y + 23);
  const int16_t dotY = mini ? (int16_t)(r.y + r.h / 2) : (int16_t)(r.y + 18);
  ui::aaFillCircle(c, (int16_t)(r.x + 16), dotY, 4, sessDotColor(s.state));

  const int16_t nameX = (int16_t)(r.x + 28);
  const int16_t nameMaxW = (int16_t)(r.x + r.w - rightReserve - nameX - 6);
  char nameBuf[48];
  const AaFont* nameFont = mini ? ui::fontCjk14() : ui::fontCjk16();
  ui::fitUtf8Line(c, nameFont, s.name, nameMaxW > 40 ? nameMaxW : 40, nameBuf, sizeof(nameBuf));
  ui::drawText(c, nameFont, nameX, row1BaseY, nameBuf,
               mini ? ui::COL_DIM : ui::COL_TEXT, bg);

  const int16_t timeRightX = (int16_t)(r.x + r.w - (selected ? 34 : 12));
  const uint16_t timeCol = waiting ? ui::COL_AMBER : ui::COL_DIM;
  ui::drawTextRight(c, ui::fontCjk14(), timeRightX, (int16_t)(row1BaseY - 1),
                    timeBuf, timeCol, bg);

  if (selected) {
    const int16_t cx = (int16_t)(r.x + r.w - 18);
    const int16_t cy = (int16_t)(r.y + 18);
    ui::aaFillCircle(c, cx, cy, 9, ui::COL_ACCENT);
    ui::drawIcon(c, kIconCheck12, (int16_t)(cx - 6), (int16_t)(cy - 6), ui::COL_BG);
  }

  if (mini) return;

  // Compact / expanded line 2: compact shows act + note/cwd; expanded puts act alone on line 2 and cwd·note on line 3.
  char line2[96] = {};
  if (expanded) {
    snprintf(line2, sizeof(line2), "%s", s.act[0] ? s.act : (s.note[0] ? s.note : "待命中"));
  } else if (s.act[0] && s.note[0]) {
    snprintf(line2, sizeof(line2), "%s · %s", s.act, s.note);
  } else if (s.act[0]) {
    snprintf(line2, sizeof(line2), "%s", s.act);
  } else if (s.cwd[0] && s.note[0]) {
    snprintf(line2, sizeof(line2), "%s · %s", s.cwd, s.note);
  } else if (s.note[0]) {
    snprintf(line2, sizeof(line2), "%s", s.note);
  } else {
    snprintf(line2, sizeof(line2), "%s", s.cwd[0] ? s.cwd : "待命中");
  }
  char line2Fit[96];
  ui::fitUtf8Line(c, ui::fontCjk14(), line2, (int16_t)(r.w - 26), line2Fit, sizeof(line2Fit));
  ui::drawText(c, ui::fontCjk14(), (int16_t)(r.x + 14), (int16_t)(r.y + 47),
               line2Fit, waiting ? ui::COL_AMBER : ui::COL_DIM, bg);

  if (!expanded) return;

  // Expanded metadata line + recent steps box
  char meta[80] = {};
  if (s.cwd[0] && s.note[0]) snprintf(meta, sizeof(meta), "%s · %s", s.cwd, s.note);
  else if (s.cwd[0]) snprintf(meta, sizeof(meta), "%s", s.cwd);
  else snprintf(meta, sizeof(meta), "%s", s.note);
  char metaFit[80];
  ui::fitUtf8Line(c, ui::fontCjk14(), meta, (int16_t)(r.w - 26), metaFit, sizeof(metaFit));
  ui::drawText(c, ui::fontCjk14(), (int16_t)(r.x + 14), (int16_t)(r.y + 68),
               metaFit, ui::COL_MUTED, bg);

  const uint8_t ns = (s.nSteps <= MAX_SESS_STEPS) ? s.nSteps : (uint8_t)MAX_SESS_STEPS;
  if (ns == 0) return;

  const int16_t boxY = (int16_t)(r.y + 78);
  const int16_t boxH = (int16_t)(10 + ns * 20);
  ui::aaFillRoundRect(c, (int16_t)(r.x + 10), boxY, (int16_t)(r.w - 20), boxH, 10,
                      ui::COL_TRACK, ui::COL_TRACK);
  for (uint8_t i = 0; i < ns; i++) {
    const BoardStep& sp = s.steps[i];
    const int16_t sy = (int16_t)(boxY + 19 + i * 20);
    if (sp.time[0]) {
      ui::drawText(c, ui::fontAscii14(), (int16_t)(r.x + 18), sy,
                   sp.time, ui::COL_MUTED, ui::COL_TRACK);
    }
    char stepDesc[64];
    if (sp.tool[0] && sp.arg[0]) snprintf(stepDesc, sizeof(stepDesc), "%s  %s", sp.tool, sp.arg);
    else snprintf(stepDesc, sizeof(stepDesc), "%s", sp.arg[0] ? sp.arg : sp.tool);
    char stepFit[64];
    ui::fitUtf8Line(c, ui::fontCjk14(), stepDesc, (int16_t)(r.w - 86), stepFit, sizeof(stepFit));
    ui::drawText(c, ui::fontCjk14(), (int16_t)(r.x + 62), sy,
                 stepFit, ui::COL_DIM, ui::COL_TRACK);
  }
}

void drawDeliveryCard(NovaCanvas& c, const NovaState& st, const ui::Rect& dr) {
  const ChatDelivery& d = st.chatDelivery;
  uint16_t bd = ui::COL_BORDER;
  uint16_t icBg = COL_SENT_IC_BG;
  uint16_t icFg = ui::COL_ACCENT;
  const AaIcon* ic = &kIconCheck12;
  char title[40] = {};

  switch (d.kind) {
    case ChatDelivKind::SENT:
      bd = ui::COL_ACCENT;
      icBg = COL_SENT_IC_BG;
      icFg = ui::COL_ACCENT;
      ic = &kIconCheck12;
      snprintf(title, sizeof(title), "已送达");
      break;
    case ChatDelivKind::RUN: {
      bd = ui::COL_AMBER;
      icBg = COL_RUN_IC_BG;
      icFg = ui::COL_AMBER;
      ic = nullptr;
      uint32_t extra = (d.updatedMs > 0 && st.nowMs >= d.updatedMs)
                         ? ((st.nowMs - d.updatedMs) / 1000u) : 0u;
      snprintf(title, sizeof(title), "在跑 · %lu 秒", (unsigned long)(d.secs + extra));
      break;
    }
    case ChatDelivKind::REPLY:
      bd = ui::COL_ACCENT;
      icBg = COL_SENT_IC_BG;
      icFg = ui::COL_ACCENT;
      ic = &kIconCheck12;
      snprintf(title, sizeof(title), "已送达");
      break;
    case ChatDelivKind::FAIL:
      bd = ui::COL_RED;
      icBg = COL_ERR_BG;
      icFg = ui::COL_RED;
      ic = &kIconX12;
      snprintf(title, sizeof(title), "未送达");
      break;
    case ChatDelivKind::MUTE:
      bd = ui::COL_MUTED;
      icBg = COL_MUTE_IC_BG;
      icFg = ui::COL_DIM;
      ic = &kIconCircleDashed14;
      snprintf(title, sizeof(title), "它没说完");
      break;
    default:
      return;
  }

  ui::aaFillRoundRect(c, dr.x, dr.y, dr.w, dr.h, 14, ui::COL_CARD, bd);

  // Badge circle + icon
  const int16_t icCx = (int16_t)(dr.x + 22);
  const int16_t icCy = (int16_t)(dr.y + 22);
  ui::aaFillCircle(c, icCx, icCy, 9, icBg);
  if (ic) {
    ui::drawIcon(c, *ic, (int16_t)(icCx - (int16_t)ic->w / 2),
                 (int16_t)(icCy - (int16_t)ic->h / 2), icFg);
  } else {
    ui::aaFillCircle(c, icCx, icCy, 4, icFg);
  }
  ui::drawText(c, ui::fontCjk14(), (int16_t)(dr.x + 38), (int16_t)(dr.y + 27),
               title, ui::COL_TEXT, ui::COL_CARD);

  if (d.kind == ChatDelivKind::REPLY) {
    char ago[20];
    fmtAgoText(d.secs, d.updatedMs, st.nowMs, ago, sizeof(ago));
    ui::drawTextRight(c, ui::fontCjk14(), (int16_t)(dr.x + dr.w - 14), (int16_t)(dr.y + 27),
                      ago, ui::COL_MUTED, ui::COL_CARD);
  }

  const int16_t bodyW = (int16_t)(dr.w - 28);
  const int16_t bodyX = (int16_t)(dr.x + 14);
  if (d.kind == ChatDelivKind::REPLY) {
    const char* txt = d.text[0] ? d.text : "已完成";
    ui::drawWrappedUtf8(c, ui::fontCjk16(), bodyX, (int16_t)(dr.y + 60),
                        bodyW, 25, 3, txt, ui::COL_TEXT, ui::COL_CARD);
    const int16_t py = (int16_t)(dr.y + dr.h - 24);
    ui::drawIcon(c, kIconPlay12, bodyX, py, ui::COL_ACCENT);
    ui::drawText(c, ui::fontCjk14(), (int16_t)(bodyX + 16), (int16_t)(py + 11),
                 "播报", ui::COL_DIM, ui::COL_CARD);
  } else {
    char body[160] = {};
    if (d.kind == ChatDelivKind::SENT) {
      if (d.queuePos > 0) snprintf(body, sizeof(body), "排队第 %u 位 · 已送入会话", (unsigned)d.queuePos);
      else snprintf(body, sizeof(body), "已进入会话队列");
    } else if (d.kind == ChatDelivKind::RUN) {
      snprintf(body, sizeof(body), "它正在处理这句话，等它说完");
    } else if (d.kind == ChatDelivKind::FAIL) {
      snprintf(body, sizeof(body), "%s", d.text[0] ? d.text : "目标会话已换段，请重选");
    } else {
      snprintf(body, sizeof(body), "%s", d.text[0] ? d.text : "这轮没有可播报的内容，不是网络问题");
    }
    ui::drawWrappedUtf8(c, ui::fontCjk14(), bodyX, (int16_t)(dr.y + 60),
                        bodyW, 23, 3, body, ui::COL_DIM, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), bodyX, (int16_t)(dr.y + dr.h - 14),
                 "点按返回看板", ui::COL_MUTED, ui::COL_CARD);
  }
}

void drawLastSayBox(NovaCanvas& c, const NovaState& st, const ui::Rect& lr) {
  ui::aaFillRoundRect(c, lr.x, lr.y, lr.w, lr.h, 12, rgb565(8, 13, 22), ui::COL_BORDER);
  char ago[20];
  fmtAgoText(st.lastSayAgeSec, st.lastSayRxMs, st.nowMs, ago, sizeof(ago));
  char hd[48];
  snprintf(hd, sizeof(hd), "上次播报 · %s · 点按重播", ago);
  ui::drawText(c, ui::fontCjk14(), (int16_t)(lr.x + 12), (int16_t)(lr.y + 21),
               hd, ui::COL_MUTED, rgb565(8, 13, 22));
  ui::drawIcon(c, kIconPlay12, (int16_t)(lr.x + lr.w - 22), (int16_t)(lr.y + 10), ui::COL_ACCENT);

  char line[128];
  ui::fitUtf8Line(c, ui::fontCjk14(), st.lastSayText, (int16_t)(lr.w - 24), line, sizeof(line));
  ui::drawText(c, ui::fontCjk14(), (int16_t)(lr.x + 12), (int16_t)(lr.y + 44),
               line, ui::COL_DIM, rgb565(8, 13, 22));
}

void drawBoardView(NovaCanvas& c, const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const uint8_t n = boardCount(st);

  if (st.chatDelivery.kind != ChatDelivKind::NONE) {
    const int sIdx = deliverySessionIndex(st);
    if (sIdx >= 0) {
      const BoardSession& bs = st.boardSessions[sIdx];
      const bool sel = (strcmp(bs.id, st.boardTargetId) == 0);
      drawSessionCard(c, st, bs, delivTargetRect(st), sel, false, false);
    }
    if (n > 1) {
      const ui::Rect fr = delivFoldRect(st);
      char fold[40];
      snprintf(fold, sizeof(fold), "另外 %u 个会话 · 点开", (unsigned)(n - 1));
      ui::drawTextCenteredInRect(c, ui::fontCjk14(), fr, fold, ui::COL_DIM, ui::COL_BG);
    }
    drawDeliveryCard(c, st, delivCardRect(st));
    return;
  }

  if (n == 0) {
    ui::aaFillRoundRect(c, (int16_t)(UI_SAFE_L + ox), (int16_t)(VIEW_TOP_BASE + 16 + oy),
                        (int16_t)UI_SAFE_W, 118, 14, ui::COL_CARD, ui::COL_BORDER);
    ui::drawTextCentered(c, ui::fontCjk16(), (int16_t)(UI_W / 2 + ox),
                         (int16_t)(VIEW_TOP_BASE + 64 + oy),
                         "命令方块就绪", ui::COL_TEXT, ui::COL_CARD);
    ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(UI_W / 2 + ox),
                         (int16_t)(VIEW_TOP_BASE + 94 + oy),
                         "暂无活跃 DSH 会话", ui::COL_DIM, ui::COL_CARD);
    if (st.lastSayText[0]) {
      drawLastSayBox(c, st, lastSayRect(st, 0));
    }
    return;
  }

  const uint8_t showCount = (n <= 3) ? n : 3;
  const int expIdx = expandedIndex(st);
  for (uint8_t i = 0; i < showCount; i++) {
    const BoardSession& bs = st.boardSessions[i];
    const bool sel = (strcmp(bs.id, st.boardTargetId) == 0);
    const bool exp = ((int)i == expIdx);
    const bool mini = (expIdx >= 0 && !exp);
    drawSessionCard(c, st, bs, boardCardRect(st, i, showCount, expIdx), sel, exp, mini);
  }

  if (expIdx < 0 && st.lastSayText[0]) {
    drawLastSayBox(c, st, lastSayRect(st, showCount));
  }
}

void drawDraftView(NovaCanvas& c, const NovaState& st) {
  const ui::Rect dc = draftCardRect(st);
  ui::aaFillRoundRect(c, dc.x, dc.y, dc.w, dc.h, 16, COL_DRAFT_BG, ui::COL_ACCENT);

  ui::drawText(c, ui::fontCjk14(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 24),
               "识别到", ui::COL_ACCENT, COL_DRAFT_BG);

  const char* dtxt = st.chatDraftText[0] ? st.chatDraftText : "...";
  ui::drawWrappedUtf8(c, ui::fontCjk16(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 52),
                      (int16_t)(dc.w - 28), 24, 3, dtxt, ui::COL_TEXT, COL_DRAFT_BG);

  const BoardSession* target = novaBoardFind(st, st.boardTargetId);
  if (target && target->state == BoardSessState::WAIT) target = nullptr;
  if (!target) target = novaBoardDefaultTarget(st);

  const ui::Rect tr = draftTargetRect(st);
  ui::aaFillRoundRect(c, tr.x, tr.y, tr.w, tr.h, 10, ui::COL_TRACK, ui::COL_BORDER);
  ui::drawText(c, ui::fontCjk14(), (int16_t)(tr.x + 10), (int16_t)(tr.y + 23),
               "发往", ui::COL_DIM, ui::COL_TRACK);

  if (target) {
    ui::aaFillCircle(c, (int16_t)(tr.x + 50), (int16_t)(tr.y + 18), 4, sessDotColor(target->state));
    char tname[48];
    ui::fitUtf8Line(c, ui::fontCjk14(), target->name, (int16_t)(tr.w - 134), tname, sizeof(tname));
    ui::drawText(c, ui::fontCjk14(), (int16_t)(tr.x + 60), (int16_t)(tr.y + 23),
                 tname, ui::COL_TEXT, ui::COL_TRACK);
  } else {
    ui::drawText(c, ui::fontCjk14(), (int16_t)(tr.x + 46), (int16_t)(tr.y + 23),
                 "暂无可用会话", ui::COL_MUTED, ui::COL_TRACK);
  }

  ui::drawTextRight(c, ui::fontCjk14(), (int16_t)(tr.x + tr.w - 22), (int16_t)(tr.y + 23),
                    "换一个", ui::COL_ACCENT, ui::COL_TRACK);
  ui::drawIcon(c, kIconChevronRight12, (int16_t)(tr.x + tr.w - 20), (int16_t)(tr.y + 12),
               ui::COL_ACCENT);

  ui::Rect rCancel{}, rPrimary{}, rSteer{};
  draftActionRects(st, target, &rCancel, &rPrimary, &rSteer);

  ui::aaFillRoundRect(c, rCancel.x, rCancel.y, rCancel.w, rCancel.h, 10,
                      ui::COL_CARD_HI, ui::COL_BORDER);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), rCancel,
                             (target && target->state == BoardSessState::RUN) ? "取消" : "取消重说",
                             ui::COL_DIM, ui::COL_CARD_HI);

  if (target && target->state == BoardSessState::RUN) {
    ui::aaFillRoundRect(c, rPrimary.x, rPrimary.y, rPrimary.w, rPrimary.h, 10,
                        ui::COL_CARD_HI, ui::COL_ACCENT);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), rPrimary, "排队", ui::COL_ACCENT, ui::COL_CARD_HI);

    ui::aaFillRoundRect(c, rSteer.x, rSteer.y, rSteer.w, rSteer.h, 10,
                        ui::COL_AMBER, ui::COL_AMBER);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), rSteer, "打断", ui::COL_BG, ui::COL_AMBER);

    ui::drawText(c, ui::fontCjk14(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 232),
                 "目标在跑 · 排队等本轮，打断立即插话", ui::COL_MUTED, COL_DRAFT_BG);
  } else if (target) {
    ui::aaFillRoundRect(c, rPrimary.x, rPrimary.y, rPrimary.w, rPrimary.h, 10,
                        ui::COL_ACCENT, ui::COL_ACCENT);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), rPrimary, "发送", ui::COL_BG, ui::COL_ACCENT);

    ui::drawText(c, ui::fontCjk14(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 232),
                 "确认无误后发送，永不自动倒计时", ui::COL_MUTED, COL_DRAFT_BG);
  } else {
    ui::aaFillRoundRect(c, rPrimary.x, rPrimary.y, rPrimary.w, rPrimary.h, 10,
                        ui::COL_TRACK, ui::COL_BORDER);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), rPrimary, "暂无会话", ui::COL_MUTED, ui::COL_TRACK);

    ui::drawText(c, ui::fontCjk14(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 232),
                 "请先在电脑端打开一个 DSH 会话", ui::COL_MUTED, COL_DRAFT_BG);
  }
}

void drawDock(NovaCanvas& c, const NovaState& st) {
  const ui::Rect r = dockBtnRect(st);
  if (st.recState == 2) {
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 24, ui::COL_CARD, ui::COL_ACCENT);
    char upBuf[24];
    snprintf(upBuf, sizeof(upBuf), "上传中 %u%%", (unsigned)st.recUploadPct);
    ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(r.x + r.w / 2), (int16_t)(r.y + 22),
                         upBuf, ui::COL_ACCENT, ui::COL_CARD);
    ui::drawProgressBar(c, (int16_t)(r.x + 22), (int16_t)(r.y + 32),
                        (int16_t)(r.w - 44), 5, st.recUploadPct, ui::COL_ACCENT, ui::COL_TRACK);
  } else if (st.recState == 3) {
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 24, ui::COL_CARD, ui::COL_AMBER);
    ui::drawTextCenteredInRect(c, ui::fontCjk16(), r, "等待回执...", ui::COL_AMBER, ui::COL_CARD);
  } else if (st.recState == 4) {
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 24, ui::COL_CARD, ui::COL_AMBER);
    ui::drawTextCenteredInRect(c, ui::fontCjk16(), r, "识别中...", ui::COL_AMBER, ui::COL_CARD);
  } else if (st.chatDraftActive) {
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 24, ui::COL_CARD, ui::COL_BORDER);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), r, "请先确认上方草稿", ui::COL_MUTED, ui::COL_CARD);
  } else if (st.recState == 5) {
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 24, COL_ERR_BG, ui::COL_RED);
    char errRaw[48], errFit[48];
    snprintf(errRaw, sizeof(errRaw), "%s · 按住重试", st.recErr[0] ? st.recErr : "识别失败");
    ui::fitUtf8Line(c, ui::fontCjk14(), errRaw, (int16_t)(r.w - 28), errFit, sizeof(errFit));
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), r, errFit, ui::COL_RED, COL_ERR_BG);
  } else {
    ui::aaFillRoundRect(c, r.x, r.y, r.w, r.h, 24, COL_HOLD_BG, ui::COL_ACCENT);
    const ui::TextMetrics tm = ui::measureText(c, ui::fontCjk16(), "按住说话");
    const int16_t gap = 8;
    const int16_t totalW = (int16_t)(kIconMic18.w + gap + tm.w);
    const int16_t startX = (int16_t)(r.x + (r.w - totalW) / 2);
    const int16_t micY = (int16_t)(r.y + (r.h - (int16_t)kIconMic18.h) / 2);
    ui::drawIcon(c, kIconMic18, startX, micY, ui::COL_ACCENT);
    ui::drawText(c, ui::fontCjk16(), (int16_t)(startX + kIconMic18.w + gap - tm.x1),
                 (int16_t)(r.y + (r.h - (int16_t)tm.h) / 2 - tm.y1),
                 "按住说话", ui::COL_ACCENT, COL_HOLD_BG);
  }
}

void drawRecordingOverlay(NovaCanvas& c, const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  c.fillScreen(ui::COL_BG);

  constexpr int kBars = 15;
  constexpr int16_t barW = 4;
  constexpr int16_t barGap = 5;
  constexpr int16_t totalW = kBars * barW + (kBars - 1) * barGap;
  const int16_t x0 = (int16_t)(UI_W / 2 - totalW / 2 + ox);
  const int16_t cy = (int16_t)(164 + oy);
  const uint16_t barCol = st.boardHoldCancel ? ui::COL_RED : ui::COL_ACCENT;
  const float phase = (float)(st.recDurMs / 90u) * 0.65f;
  const float amp = 0.25f + 0.75f * ((float)(st.recLevel > 100 ? 100 : st.recLevel) / 100.0f);

  for (int i = 0; i < kBars; i++) {
    const float centerWeight = 1.0f - fabsf((float)i - 7.0f) / 8.5f;
    const float wave = 0.55f + 0.45f * fabsf(sinf((float)i * 0.85f + phase));
    int16_t h = (int16_t)(8 + lroundf(44.0f * amp * centerWeight * wave));
    if (h < 8) h = 8;
    if (h > 54) h = 54;
    ui::aaFillRoundRect(c, (int16_t)(x0 + i * (barW + barGap)), (int16_t)(cy - h / 2),
                        barW, h, 2, barCol, barCol);
  }

  uint32_t durMs = st.recDurMs;
  if (durMs > 10000u) durMs = 10000u;
  char durBuf[16];
  snprintf(durBuf, sizeof(durBuf), "%u.%u", (unsigned)(durMs / 1000u),
           (unsigned)((durMs % 1000u) / 100u));
  ui::drawTextCentered(c, ui::fontNum46(), (int16_t)(UI_W / 2 + ox), (int16_t)(250 + oy),
                       durBuf, ui::COL_TEXT, ui::COL_BG);

  ui::drawTextCentered(c, ui::fontCjk16(), (int16_t)(UI_W / 2 + ox), (int16_t)(294 + oy),
                       st.boardHoldCancel ? "松开取消" : "松开发送 · 上滑取消",
                       st.boardHoldCancel ? ui::COL_RED : ui::COL_DIM, ui::COL_BG);

  ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(UI_W / 2 + ox), (int16_t)(322 + oy),
                       "最长 10 秒", ui::COL_MUTED, ui::COL_BG);
}

void drawToast(NovaCanvas& c, const NovaState& st) {
  if (!st.boardToast[0]) return;
  if (st.nowMs < st.boardToastStartMs || (st.nowMs - st.boardToastStartMs) >= 1500u) return;
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const ui::TextMetrics tm = ui::measureText(c, ui::fontCjk14(), st.boardToast);
  int16_t tw = (int16_t)(tm.w + 28);
  if (tw < 96) tw = 96;
  if (tw > UI_SAFE_W) tw = UI_SAFE_W;
  const ui::Rect tr{ (int16_t)((UI_W - tw) / 2 + ox), (int16_t)(66 + oy), tw, 28 };
  ui::aaFillRoundRect(c, tr.x, tr.y, tr.w, tr.h, 14, ui::COL_CARD_HI, ui::COL_BORDER);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), tr, st.boardToast, ui::COL_TEXT, ui::COL_CARD_HI);
}

// ══════════════════════════════════════════════════════════════════════
// 潮汐（TIDE）命令看板
// 契约 §4⑦：几何命令方块（零圆角）+ 待批红块 + 反白展开 + 夜汐录音 + 白纸回执。
// 所有矩形全部复用原版的 boardCardRect / dockBtnRect / draftCardRect /
// delivCardRect / draftActionRects，保证命中判定两种主题完全一致。
// ══════════════════════════════════════════════════════════════════════
namespace tide_draw {

void drawHeaderTide(NovaCanvas& c, const NovaState& st, const BoardSession* target) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);

  c.fillRect(0, 38, UI_W, (int16_t)(VIEW_TOP_BASE + oy - 38), ui::tide::BG_COBALT);
  ui::tide::drawTopStrip(c, st, 5, ui::tide::BG_COBALT);

  const char* hdTitle = st.chatDraftActive ? "确认一下"
                       : (st.chatDelivery.kind != ChatDelivKind::NONE ? "已送达" : "命令看板");
  ui::drawText(c, ui::fontCjk16(), (int16_t)(UI_SAFE_L + 2 + ox), (int16_t)(78 + oy),
               hdTitle, ui::tide::WHITE, ui::tide::BG_COBALT);

  char sub[72] = {};
  uint16_t subCol = ui::tide::WHITE_DIM;
  if (st.chatDraftActive) {
    snprintf(sub, sizeof(sub), "说完了 · 确认一下");
  } else if (target) {
    char rawSub[72];
    snprintf(rawSub, sizeof(rawSub), "发往 · %s", target->name);
    ui::fitUtf8Line(c, ui::fontCjk14(), rawSub, (int16_t)(UI_SAFE_W - 114), sub, sizeof(sub));
    subCol = ui::tide::WHITE;
  } else {
    snprintf(sub, sizeof(sub), "全局 · 只读巡检");
  }
  ui::drawText(c, ui::fontCjk14(), (int16_t)(UI_SAFE_L + 2 + ox), (int16_t)(98 + oy),
               sub, subCol, ui::tide::BG_COBALT);

  // 右侧反白状态章：待批 -> 绯红实心 + 纯白细框，其余 -> 纯白实心
  char badgeTxt[24] = {};
  uint16_t ignoreBg = 0, ignoreBd = 0, ignoreFg = 0;
  bool hasWait = false;
  computeHeaderBadge(st, badgeTxt, sizeof(badgeTxt), &ignoreBg, &ignoreBd, &ignoreFg, &hasWait);
  const ui::Rect br = headerBadgeRect(st, badgeTxt);
  const uint16_t chipBg = hasWait ? ui::tide::BG_CRIMSON : ui::tide::WHITE;
  c.fillRect(br.x, br.y, br.w, br.h, chipBg);
  if (hasWait) {
    c.drawRect(br.x, br.y, br.w, br.h, ui::tide::WHITE);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), br, badgeTxt, ui::tide::WHITE, chipBg);
  } else {
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), br, badgeTxt, ui::tide::BG_COBALT, chipBg);
  }
  // 页头底部分割线，与下方方块群拉开清晰层次
  c.fillRect(UI_SAFE_L + ox, (int16_t)(VIEW_TOP_BASE - 4 + oy), UI_SAFE_W, 1, ui::tide::WHITE_LINE);
}

void drawSessionCardTide(NovaCanvas& c, const NovaState& st, const BoardSession& s,
                         ui::Rect r, bool selected, bool expanded, bool mini) {
  const bool waiting = (s.state == BoardSessState::WAIT);
  const bool running = (s.state == BoardSessState::RUN);

  // 底色映射：展开卡反白为纯白，WAIT 整块绯红，RUN 深靛蓝，IDLE 轻蓝。
  uint16_t bg = ui::tide::SURF_SUB;
  uint16_t fgMain = ui::tide::WHITE;
  uint16_t fgDim = ui::tide::WHITE_DIM;
  if (expanded) {
    bg = ui::tide::WHITE;
    fgMain = ui::tide::SURF_INK;
    fgDim = ui::tide::MUTED_ON_W;
  } else if (waiting) {
    bg = ui::tide::BG_CRIMSON;
  } else if (running) {
    bg = ui::tide::SURF_DEEP;
  }

  // 0px 圆角几何方块（去外黑框，白卡靠纯净白面 + 左侧 5px 深靛蓝竖标立住）
  c.fillRect(r.x, r.y, r.w, r.h, bg);

  // 左侧 5px 竖标：WAIT / RUN / 选中 都要
  if (waiting || running || selected) {
    const uint16_t barCol = expanded ? ui::tide::SURF_INK : ui::tide::WHITE;
    c.fillRect(r.x, r.y, 5, r.h, barCol);
  }

  char timeBuf[24];
  fmtSessTime(s, st.nowMs, st.boardRxMs, timeBuf, sizeof(timeBuf));
  const uint16_t timeW = ui::measureText(c, ui::fontCjk14(), timeBuf).w;
  const int16_t rightReserve = (int16_t)(timeW + (selected && !expanded ? 38 : 16));

  const int16_t row1BaseY = mini ? (int16_t)(r.y + 24) : (int16_t)(r.y + 25);
  int16_t nameX = (int16_t)(r.x + 16);
  // 严格对齐设计稿：状态标志放在会话名左侧（在跑带实心圆点、待批带 !），右侧保持纯文字读数。
  if (running) {
    const uint16_t dotCol = expanded ? ui::tide::BG_COBALT : ui::tide::WHITE;
    ui::aaFillCircle(c, (int16_t)(nameX + 3), (int16_t)(row1BaseY - 6), 3, dotCol);
    nameX = (int16_t)(nameX + 12);
  } else if (waiting) {
    ui::drawText(c, ui::fontAscii18(), nameX, row1BaseY, "!", ui::tide::WHITE, bg);
    nameX = (int16_t)(nameX + 12);
  }

  const int16_t nameMaxW = (int16_t)(r.x + r.w - rightReserve - nameX - 6);
  char nameBuf[48];
  const AaFont* nameFont = mini ? ui::fontCjk14() : ui::fontCjk16();
  ui::fitUtf8Line(c, nameFont, s.name, nameMaxW > 40 ? nameMaxW : 40, nameBuf, sizeof(nameBuf));
  ui::drawText(c, nameFont, nameX, row1BaseY, nameBuf, fgMain, bg);

  const int16_t timeRightX = (int16_t)(r.x + r.w - (selected && !expanded ? 34 : 12));
  ui::drawTextRight(c, ui::fontCjk14(), timeRightX, (int16_t)(row1BaseY - 1),
                    timeBuf, waiting ? ui::tide::WHITE : fgDim, bg);

  if (selected && !expanded) {
    const int16_t cx = (int16_t)(r.x + r.w - 16);
    const int16_t cy = (int16_t)(r.y + 18);
    c.fillRect((int16_t)(cx - 9), (int16_t)(cy - 9), 18, 18, ui::tide::WHITE);
    ui::drawIcon(c, kIconCheck12, (int16_t)(cx - 6), (int16_t)(cy - 6), ui::tide::BG_COBALT);
  }

  if (mini) return;

  char line2[96] = {};
  if (expanded) {
    snprintf(line2, sizeof(line2), "%s", s.act[0] ? s.act : (s.note[0] ? s.note : "待命中"));
  } else if (s.act[0] && s.note[0]) {
    snprintf(line2, sizeof(line2), "%s · %s", s.act, s.note);
  } else if (s.act[0]) {
    snprintf(line2, sizeof(line2), "%s", s.act);
  } else if (s.cwd[0] && s.note[0]) {
    snprintf(line2, sizeof(line2), "%s · %s", s.cwd, s.note);
  } else if (s.note[0]) {
    snprintf(line2, sizeof(line2), "%s", s.note);
  } else {
    snprintf(line2, sizeof(line2), "%s", s.cwd[0] ? s.cwd : "待命中");
  }
  char line2Fit[96];
  ui::fitUtf8Line(c, ui::fontCjk14(), line2, (int16_t)(r.w - 30), line2Fit, sizeof(line2Fit));
  ui::drawText(c, ui::fontCjk14(), (int16_t)(r.x + 16), (int16_t)(r.y + 47),
               line2Fit, waiting ? ui::tide::dimOn(ui::tide::BG_CRIMSON) : fgDim, bg);

  if (!expanded) return;

  char meta[80] = {};
  if (s.cwd[0] && s.note[0]) snprintf(meta, sizeof(meta), "%s · %s", s.cwd, s.note);
  else if (s.cwd[0]) snprintf(meta, sizeof(meta), "%s", s.cwd);
  else snprintf(meta, sizeof(meta), "%s", s.note);
  char metaFit[80];
  ui::fitUtf8Line(c, ui::fontCjk14(), meta, (int16_t)(r.w - 30), metaFit, sizeof(metaFit));
  ui::drawText(c, ui::fontCjk14(), (int16_t)(r.x + 16), (int16_t)(r.y + 68),
               metaFit, ui::tide::MUTED_ON_W, bg);

  const uint8_t ns = (s.nSteps <= MAX_SESS_STEPS) ? s.nSteps : (uint8_t)MAX_SESS_STEPS;
  if (ns == 0) return;

  // 步骤区：嵌 SURF_INK 深蓝终端盒，纯白等宽文本，左右对齐 16px。
  const int16_t boxY = (int16_t)(r.y + 78);
  const int16_t boxH = (int16_t)(10 + ns * 20);
  c.fillRect((int16_t)(r.x + 14), boxY, (int16_t)(r.w - 26), boxH, ui::tide::SURF_INK);
  for (uint8_t i = 0; i < ns; i++) {
    const BoardStep& sp = s.steps[i];
    const int16_t sy = (int16_t)(boxY + 19 + i * 20);
    if (sp.time[0]) {
      ui::drawText(c, ui::fontAscii14(), (int16_t)(r.x + 18), sy,
                   sp.time, ui::tide::WHITE_DIM, ui::tide::SURF_INK);
    }
    char stepDesc[64];
    if (sp.tool[0] && sp.arg[0]) snprintf(stepDesc, sizeof(stepDesc), "%s  %s", sp.tool, sp.arg);
    else snprintf(stepDesc, sizeof(stepDesc), "%s", sp.arg[0] ? sp.arg : sp.tool);
    char stepFit[64];
    ui::fitUtf8Line(c, ui::fontCjk14(), stepDesc, (int16_t)(r.w - 86), stepFit, sizeof(stepFit));
    ui::drawText(c, ui::fontCjk14(), (int16_t)(r.x + 62), sy,
                 stepFit, ui::tide::WHITE, ui::tide::SURF_INK);
  }
}

void drawLastSayBoxTide(NovaCanvas& c, const NovaState& st, const ui::Rect& lr) {
  c.fillRect(lr.x, lr.y, lr.w, lr.h, ui::tide::SURF_DEEP);
  char ago[20];
  fmtAgoText(st.lastSayAgeSec, st.lastSayRxMs, st.nowMs, ago, sizeof(ago));
  char hd[48];
  snprintf(hd, sizeof(hd), "上次播报 · %s · 点按重播", ago);
  ui::drawText(c, ui::fontCjk14(), (int16_t)(lr.x + 12), (int16_t)(lr.y + 21),
               hd, ui::tide::WHITE_DIM, ui::tide::SURF_DEEP);
  ui::drawIcon(c, kIconPlay12, (int16_t)(lr.x + lr.w - 22), (int16_t)(lr.y + 10), ui::tide::WHITE);

  char line[128];
  ui::fitUtf8Line(c, ui::fontCjk14(), st.lastSayText, (int16_t)(lr.w - 24), line, sizeof(line));
  ui::drawText(c, ui::fontCjk14(), (int16_t)(lr.x + 12), (int16_t)(lr.y + 44),
               line, ui::tide::WHITE, ui::tide::SURF_DEEP);
}

void drawDeliveryCardTide(NovaCanvas& c, const NovaState& st, const ui::Rect& dr) {
  const ChatDelivery& d = st.chatDelivery;

  char title[40] = {};
  uint16_t accentBar = ui::tide::WHITE;
  switch (d.kind) {
    case ChatDelivKind::SENT:
      snprintf(title, sizeof(title), "已送达");
      break;
    case ChatDelivKind::RUN: {
      uint32_t extra = (d.updatedMs > 0 && st.nowMs >= d.updatedMs)
                         ? ((st.nowMs - d.updatedMs) / 1000u) : 0u;
      snprintf(title, sizeof(title), "在跑 · %lu 秒", (unsigned long)(d.secs + extra));
      break;
    }
    case ChatDelivKind::REPLY:
      snprintf(title, sizeof(title), "已送达");
      break;
    case ChatDelivKind::FAIL:
      snprintf(title, sizeof(title), "未送达");
      accentBar = ui::tide::BG_CRIMSON;
      break;
    case ChatDelivKind::MUTE:
      snprintf(title, sizeof(title), "它没说完");
      accentBar = ui::tide::MUTED_ON_W;
      break;
    default:
      return;
  }

  // 白纸卡：纯白底 + SURF_INK 深蓝大字，左侧 5px 状态竖标。
  c.fillRect(dr.x, dr.y, dr.w, dr.h, ui::tide::WHITE);
  c.fillRect(dr.x, dr.y, 5, dr.h, accentBar);

  ui::drawText(c, ui::fontCjk16(), (int16_t)(dr.x + 16), (int16_t)(dr.y + 30),
               title, ui::tide::SURF_INK, ui::tide::WHITE);

  if (d.kind == ChatDelivKind::REPLY) {
    char ago[20];
    fmtAgoText(d.secs, d.updatedMs, st.nowMs, ago, sizeof(ago));
    ui::drawTextRight(c, ui::fontCjk14(), (int16_t)(dr.x + dr.w - 14), (int16_t)(dr.y + 30),
                      ago, ui::tide::MUTED_ON_W, ui::tide::WHITE);
  }

  const int16_t bodyW = (int16_t)(dr.w - 32);
  const int16_t bodyX = (int16_t)(dr.x + 16);
  if (d.kind == ChatDelivKind::REPLY) {
    const char* txt = d.text[0] ? d.text : "已完成";
    ui::drawWrappedUtf8(c, ui::fontCjk16(), bodyX, (int16_t)(dr.y + 62),
                        bodyW, 25, 3, txt, ui::tide::SURF_INK, ui::tide::WHITE);
    const int16_t py = (int16_t)(dr.y + dr.h - 26);
    ui::drawIcon(c, kIconPlay12, bodyX, py, ui::tide::BG_COBALT);
    ui::drawText(c, ui::fontCjk14(), (int16_t)(bodyX + 16), (int16_t)(py + 11),
                 "播报", ui::tide::BG_COBALT, ui::tide::WHITE);
  } else {
    char body[160] = {};
    if (d.kind == ChatDelivKind::SENT) {
      if (d.queuePos > 0) snprintf(body, sizeof(body), "排队第 %u 位 · 已送入会话", (unsigned)d.queuePos);
      else snprintf(body, sizeof(body), "已进入会话队列");
    } else if (d.kind == ChatDelivKind::RUN) {
      snprintf(body, sizeof(body), "它正在处理这句话，等它说完");
    } else if (d.kind == ChatDelivKind::FAIL) {
      snprintf(body, sizeof(body), "%s", d.text[0] ? d.text : "目标会话已换段，请重选");
    } else {
      snprintf(body, sizeof(body), "%s", d.text[0] ? d.text : "这轮没有可播报的内容，不是网络问题");
    }
    ui::drawWrappedUtf8(c, ui::fontCjk14(), bodyX, (int16_t)(dr.y + 62),
                        bodyW, 23, 3, body, ui::tide::MUTED_ON_W, ui::tide::WHITE);
    ui::drawText(c, ui::fontCjk14(), bodyX, (int16_t)(dr.y + dr.h - 14),
                 "点按返回看板", ui::tide::MUTED_ON_W, ui::tide::WHITE);
  }
}

void drawBoardViewTide(NovaCanvas& c, const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const uint8_t n = boardCount(st);

  if (st.chatDelivery.kind != ChatDelivKind::NONE) {
    const int sIdx = deliverySessionIndex(st);
    if (sIdx >= 0) {
      const BoardSession& bs = st.boardSessions[sIdx];
      const bool sel = (strcmp(bs.id, st.boardTargetId) == 0);
      drawSessionCardTide(c, st, bs, delivTargetRect(st), sel, false, false);
    }
    if (n > 1) {
      const ui::Rect fr = delivFoldRect(st);
      char fold[40];
      snprintf(fold, sizeof(fold), "另外 %u 个会话 · 点开", (unsigned)(n - 1));
      ui::drawTextCenteredInRect(c, ui::fontCjk14(), fr, fold,
                                 ui::tide::WHITE_DIM, ui::tide::BG_COBALT);
    }
    drawDeliveryCardTide(c, st, delivCardRect(st));
    return;
  }

  if (n == 0) {
    const ui::Rect emptyR{ (int16_t)(UI_SAFE_L + ox), (int16_t)(VIEW_TOP_BASE + 16 + oy),
                           (int16_t)UI_SAFE_W, 118 };
    c.fillRect(emptyR.x, emptyR.y, emptyR.w, emptyR.h, ui::tide::SURF_DEEP);
    ui::drawTextCentered(c, ui::fontCjk16(), (int16_t)(UI_W / 2 + ox),
                         (int16_t)(VIEW_TOP_BASE + 64 + oy),
                         "命令看板就绪", ui::tide::WHITE, ui::tide::SURF_DEEP);
    ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(UI_W / 2 + ox),
                         (int16_t)(VIEW_TOP_BASE + 94 + oy),
                         "暂无活跃 DSH 会话", ui::tide::WHITE_DIM, ui::tide::SURF_DEEP);
    if (st.lastSayText[0]) drawLastSayBoxTide(c, st, lastSayRect(st, 0));
    return;
  }

  const uint8_t showCount = (n <= 3) ? n : 3;
  const int expIdx = expandedIndex(st);
  for (uint8_t i = 0; i < showCount; i++) {
    const BoardSession& bs = st.boardSessions[i];
    const bool sel = (strcmp(bs.id, st.boardTargetId) == 0);
    const bool exp = ((int)i == expIdx);
    const bool mini = (expIdx >= 0 && !exp);
    drawSessionCardTide(c, st, bs, boardCardRect(st, i, showCount, expIdx), sel, exp, mini);
  }

  if (expIdx < 0 && st.lastSayText[0]) {
    drawLastSayBoxTide(c, st, lastSayRect(st, showCount));
  }
}

void drawDraftViewTide(NovaCanvas& c, const NovaState& st) {
  const ui::Rect dc = draftCardRect(st);
  // 白纸确认卡
  c.fillRect(dc.x, dc.y, dc.w, dc.h, ui::tide::WHITE);

  ui::drawText(c, ui::fontCjk14(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 24),
               "识别到", ui::tide::BG_COBALT, ui::tide::WHITE);

  const char* dtxt = st.chatDraftText[0] ? st.chatDraftText : "...";
  ui::drawWrappedUtf8(c, ui::fontCjk16(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 52),
                      (int16_t)(dc.w - 28), 24, 3, dtxt, ui::tide::SURF_INK, ui::tide::WHITE);

  const BoardSession* target = novaBoardFind(st, st.boardTargetId);
  if (target && target->state == BoardSessState::WAIT) target = nullptr;
  if (!target) target = novaBoardDefaultTarget(st);

  const ui::Rect tr = draftTargetRect(st);
  c.fillRect(tr.x, tr.y, tr.w, tr.h, ui::tide::SURF_TINT);
  ui::drawText(c, ui::fontCjk14(), (int16_t)(tr.x + 10), (int16_t)(tr.y + 23),
               "发往", ui::tide::MUTED_ON_W, ui::tide::SURF_TINT);

  if (target) {
    c.fillRect((int16_t)(tr.x + 48), (int16_t)(tr.y + 15), 6, 6,
               target->state == BoardSessState::WAIT ? ui::tide::BG_CRIMSON
                                                     : ui::tide::BG_COBALT);
    char tname[48];
    ui::fitUtf8Line(c, ui::fontCjk14(), target->name, (int16_t)(tr.w - 134), tname, sizeof(tname));
    ui::drawText(c, ui::fontCjk14(), (int16_t)(tr.x + 60), (int16_t)(tr.y + 23),
                 tname, ui::tide::SURF_INK, ui::tide::SURF_TINT);
  } else {
    ui::drawText(c, ui::fontCjk14(), (int16_t)(tr.x + 46), (int16_t)(tr.y + 23),
                 "暂无可用会话", ui::tide::MUTED_ON_W, ui::tide::SURF_TINT);
  }

  ui::drawTextRight(c, ui::fontCjk14(), (int16_t)(tr.x + tr.w - 22), (int16_t)(tr.y + 23),
                    "换一个", ui::tide::BG_COBALT, ui::tide::SURF_TINT);
  ui::drawIcon(c, kIconChevronRight12, (int16_t)(tr.x + tr.w - 20), (int16_t)(tr.y + 12),
               ui::tide::BG_COBALT);

  ui::Rect rCancel{}, rPrimary{}, rSteer{};
  draftActionRects(st, target, &rCancel, &rPrimary, &rSteer);

  // 重说：深蓝细框
  c.drawRect(rCancel.x, rCancel.y, rCancel.w, rCancel.h, ui::tide::SURF_INK);
  c.drawRect(rCancel.x + 1, rCancel.y + 1, rCancel.w - 2, rCancel.h - 2, ui::tide::SURF_INK);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), rCancel,
                             (target && target->state == BoardSessState::RUN) ? "取消" : "重说",
                             ui::tide::SURF_INK, ui::tide::WHITE);

  if (target && target->state == BoardSessState::RUN) {
    // 排队：钴蓝实心
    c.fillRect(rPrimary.x, rPrimary.y, rPrimary.w, rPrimary.h, ui::tide::BG_COBALT);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), rPrimary, "排队",
                               ui::tide::WHITE, ui::tide::BG_COBALT);
    // 打断：绯红实心
    c.fillRect(rSteer.x, rSteer.y, rSteer.w, rSteer.h, ui::tide::BG_CRIMSON);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), rSteer, "打断",
                               ui::tide::WHITE, ui::tide::BG_CRIMSON);
    ui::drawText(c, ui::fontCjk14(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 232),
                 "目标在跑 · 排队等本轮，打断立即插话",
                 ui::tide::MUTED_ON_W, ui::tide::WHITE);
  } else if (target) {
    c.fillRect(rPrimary.x, rPrimary.y, rPrimary.w, rPrimary.h, ui::tide::BG_COBALT);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), rPrimary, "发送",
                               ui::tide::WHITE, ui::tide::BG_COBALT);
    ui::drawText(c, ui::fontCjk14(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 232),
                 "确认无误后发送，永不自动倒计时",
                 ui::tide::MUTED_ON_W, ui::tide::WHITE);
  } else {
    c.fillRect(rPrimary.x, rPrimary.y, rPrimary.w, rPrimary.h, ui::tide::SURF_TINT);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), rPrimary, "暂无会话",
                               ui::tide::MUTED_ON_W, ui::tide::SURF_TINT);
    ui::drawText(c, ui::fontCjk14(), (int16_t)(dc.x + 14), (int16_t)(dc.y + 232),
                 "请先在电脑端打开一个 DSH 会话",
                 ui::tide::MUTED_ON_W, ui::tide::WHITE);
  }
}

void drawDockTide(NovaCanvas& c, const NovaState& st) {
  const ui::Rect r = dockBtnRect(st);
  if (st.recState == 2) {
    c.fillRect(r.x, r.y, r.w, r.h, ui::tide::WHITE);
    char upBuf[24];
    snprintf(upBuf, sizeof(upBuf), "上传中 %u%%", (unsigned)st.recUploadPct);
    ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(r.x + r.w / 2), (int16_t)(r.y + 22),
                         upBuf, ui::tide::SURF_INK, ui::tide::WHITE);
    const int16_t bx = (int16_t)(r.x + 22);
    const int16_t bw = (int16_t)(r.w - 44);
    c.fillRect(bx, (int16_t)(r.y + 32), bw, 5, ui::tide::SURF_TINT);
    int16_t fw = (int16_t)(((int32_t)bw * st.recUploadPct + 50) / 100);
    if (fw > bw) fw = bw;
    if (fw > 0) c.fillRect(bx, (int16_t)(r.y + 32), fw, 5, ui::tide::BG_COBALT);
  } else if (st.recState == 3 || st.recState == 4) {
    c.drawRect(r.x, r.y, r.w, r.h, ui::tide::WHITE);
    c.drawRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, ui::tide::WHITE);
    ui::drawTextCenteredInRect(c, ui::fontCjk16(), r,
                               st.recState == 3 ? "等待回执..." : "识别中...",
                               ui::tide::WHITE, ui::tide::BG_COBALT);
  } else if (st.chatDraftActive) {
    c.drawRect(r.x, r.y, r.w, r.h, ui::tide::WHITE_LINE);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), r, "请先确认上方草稿",
                               ui::tide::WHITE_DIM, ui::tide::BG_COBALT);
  } else if (st.recState == 5) {
    c.fillRect(r.x, r.y, r.w, r.h, ui::tide::BG_CRIMSON);
    char errRaw[48], errFit[48];
    snprintf(errRaw, sizeof(errRaw), "%s · 按住重试", st.recErr[0] ? st.recErr : "识别失败");
    ui::fitUtf8Line(c, ui::fontCjk14(), errRaw, (int16_t)(r.w - 28), errFit, sizeof(errFit));
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), r, errFit,
                               ui::tide::WHITE, ui::tide::BG_CRIMSON);
  } else {
    // 纯白实心通栏大块 + 钴蓝粗字
    c.fillRect(r.x, r.y, r.w, r.h, ui::tide::WHITE);
    const ui::TextMetrics tm = ui::measureText(c, ui::fontCjk16(), "按住说话");
    constexpr int16_t gap = 8;
    const int16_t totalW = (int16_t)(kIconMic18.w + gap + tm.w);
    const int16_t startX = (int16_t)(r.x + (r.w - totalW) / 2);
    const int16_t micY = (int16_t)(r.y + (r.h - (int16_t)kIconMic18.h) / 2);
    ui::drawIcon(c, kIconMic18, startX, micY, ui::tide::BG_COBALT);
    ui::drawText(c, ui::fontCjk16(), (int16_t)(startX + kIconMic18.w + gap - tm.x1),
                 (int16_t)(r.y + (r.h - (int16_t)tm.h) / 2 - tm.y1),
                 "按住说话", ui::tide::BG_COBALT, ui::tide::WHITE);
  }
}

void drawRecordingOverlayTide(NovaCanvas& c, const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  // 录音底：夜汐；上滑取消切信号绯红。
  const uint16_t bg = st.boardHoldCancel ? ui::tide::BG_CRIMSON : ui::tide::BG_NIGHT;
  const uint16_t dim = ui::tide::dimOn(bg);
  c.fillScreen(bg);

  ui::drawTextCentered(c, ui::fontAscii14(), (int16_t)(UI_W / 2 + ox), (int16_t)(82 + oy),
                       st.boardHoldCancel ? "RELEASE TO CANCEL" : "RECORDING // VOICE", dim, bg);

  constexpr int kBars = 15;
  constexpr int16_t barW = 5;
  constexpr int16_t barGap = 5;
  const int16_t totalW = (int16_t)(kBars * barW + (kBars - 1) * barGap);
  const int16_t x0 = (int16_t)(UI_W / 2 - totalW / 2 + ox);
  const int16_t cy = (int16_t)(152 + oy);
  const uint16_t barCol = ui::tide::WHITE;
  const float phase = (float)(st.recDurMs / 90u) * 0.65f;
  const float amp = 0.25f + 0.75f * ((float)(st.recLevel > 100 ? 100 : st.recLevel) / 100.0f);

  for (int i = 0; i < kBars; i++) {
    const float centerWeight = 1.0f - fabsf((float)i - 7.0f) / 8.5f;
    const float wave = 0.55f + 0.45f * fabsf(sinf((float)i * 0.85f + phase));
    int16_t h = (int16_t)(10 + lroundf(52.0f * amp * centerWeight * wave));
    if (h < 10) h = 10;
    if (h > 62) h = 62;
    c.fillRect((int16_t)(x0 + i * (barW + barGap)), (int16_t)(cy - h / 2), barW, h, barCol);
  }

  uint32_t durMs = st.recDurMs;
  if (durMs > 10000u) durMs = 10000u;
  char durBuf[16];
  snprintf(durBuf, sizeof(durBuf), "%u.%u", (unsigned)(durMs / 1000u),
           (unsigned)((durMs % 1000u) / 100u));
  ui::drawTextCentered(c, ui::fontNum92(), (int16_t)(UI_W / 2 + ox), (int16_t)(286 + oy),
                       durBuf, ui::tide::WHITE, bg);

  ui::drawTextCentered(c, ui::fontCjk16(), (int16_t)(UI_W / 2 + ox), (int16_t)(334 + oy),
                       st.boardHoldCancel ? "松开手指即作废" : "松开发送 · 上滑取消",
                       ui::tide::WHITE, bg);

  ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(UI_W / 2 + ox), (int16_t)(362 + oy),
                       "最长 10 秒 · 绝不自动发送", dim, bg);
}

void drawToastTide(NovaCanvas& c, const NovaState& st) {
  if (!st.boardToast[0]) return;
  if (st.nowMs < st.boardToastStartMs || (st.nowMs - st.boardToastStartMs) >= 1500u) return;
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const ui::TextMetrics tm = ui::measureText(c, ui::fontCjk14(), st.boardToast);
  int16_t tw = (int16_t)(tm.w + 28);
  if (tw < 96) tw = 96;
  if (tw > UI_SAFE_W) tw = UI_SAFE_W;
  const ui::Rect tr{ (int16_t)((UI_W - tw) / 2 + ox), (int16_t)(66 + oy), tw, 28 };
  c.fillRect(tr.x, tr.y, tr.w, tr.h, ui::tide::WHITE);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), tr, st.boardToast,
                             ui::tide::SURF_INK, ui::tide::WHITE);
}

void drawChatTide(NovaCanvas& c, const NovaState& st, const BoardSession* target) {
  const uint16_t bg = (st.recState == 1 && st.boardHoldCancel)
                        ? ui::tide::BG_CRIMSON
                        : (st.recState == 1 ? ui::tide::BG_NIGHT : ui::tide::BG_COBALT);
  c.fillScreen(bg);
  drawHeaderTide(c, st, target);
  if (st.chatDraftActive) drawDraftViewTide(c, st);
  else                    drawBoardViewTide(c, st);
  drawDockTide(c, st);
  drawToastTide(c, st);
}

}  // namespace tide_draw

}  // namespace

const char* pageChatQuickCmdText(int) {
  return "";
}

bool pageChatIsHoldBtn(int16_t x, int16_t y, const NovaState& st) {
  if (st.recState == 1 || st.chatDraftActive) return false;
  return dockBtnRect(st).contains(x, y);
}

bool pageChatCanStartHold(const NovaState& st) {
  return !st.chatDraftActive && st.recState != 1 && st.recState != 2;
}

void pageChatDraw(NovaCanvas& c, const NovaState& st) {
  if (ui::isTideTheme()) {
    if (st.recState == 1) {
      tide_draw::drawRecordingOverlayTide(c, st);
      return;
    }
    const BoardSession* tgt = novaBoardFind(st, st.boardTargetId);
    tide_draw::drawChatTide(c, st, tgt);
    return;
  }

  if (st.recState == 1) {
    drawRecordingOverlay(c, st);
    return;
  }

  c.fillScreen(ui::COL_BG);
  const BoardSession* target = novaBoardFind(st, st.boardTargetId);
  drawHeader(c, st, target);

  if (st.chatDraftActive) {
    drawDraftView(c, st);
  } else {
    drawBoardView(c, st);
  }

  drawDock(c, st);
  drawToast(c, st);
}

UiAction pageChatTap(int16_t x, int16_t y, const NovaState& st) {
  if (st.recState == 1) return UiAction{};

  if (st.chatDraftActive) {
    if (draftTargetRect(st).contains(x, y)) {
      return UiAction{ UiAction::CHAT_DRAFT_CYCLE_TARGET, 0 };
    }
    const BoardSession* target = novaBoardFind(st, st.boardTargetId);
    if (target && target->state == BoardSessState::WAIT) target = nullptr;
    if (!target) target = novaBoardDefaultTarget(st);

    ui::Rect rCancel{}, rPrimary{}, rSteer{};
    draftActionRects(st, target, &rCancel, &rPrimary, &rSteer);
    if (rCancel.contains(x, y)) return UiAction{ UiAction::CHAT_DRAFT_CANCEL, 0 };
    if (target && rPrimary.contains(x, y)) return UiAction{ UiAction::CHAT_DRAFT_SEND_MODE, 0 };
    if (target && target->state == BoardSessState::RUN && rSteer.contains(x, y)) {
      return UiAction{ UiAction::CHAT_DRAFT_SEND_MODE, 1 };
    }
    return UiAction{};
  }

  char badgeTxt[24] = {};
  uint16_t bg = 0, bd = 0, fg = 0;
  bool hasWait = false;
  computeHeaderBadge(st, badgeTxt, sizeof(badgeTxt), &bg, &bd, &fg, &hasWait);
  if (hasWait && headerBadgeRect(st, badgeTxt).contains(x, y)) {
    return UiAction{ UiAction::CHAT_OPEN_APPROVAL, 0 };
  }

  if (st.chatDelivery.kind != ChatDelivKind::NONE) {
    if (boardCount(st) > 0 && delivTargetRect(st).contains(x, y)) {
      return UiAction{ UiAction::CHAT_DELIVERY_DISMISS, 0 };
    }
    if (boardCount(st) > 1 && delivFoldRect(st).contains(x, y)) {
      return UiAction{ UiAction::CHAT_DELIVERY_DISMISS, 0 };
    }
    if (delivCardRect(st).contains(x, y)) {
      if (st.chatDelivery.kind == ChatDelivKind::REPLY) {
        return UiAction{ UiAction::CHAT_REPLAY_LAST, 0 };
      }
      return UiAction{ UiAction::CHAT_DELIVERY_DISMISS, 0 };
    }
    return UiAction{};
  }

  const uint8_t n = boardCount(st);
  const uint8_t showCount = (n <= 3) ? n : 3;
  const int expIdx = expandedIndex(st);
  for (uint8_t i = 0; i < showCount; i++) {
    if (boardCardRect(st, i, showCount, expIdx).contains(x, y)) {
      return UiAction{ UiAction::CHAT_SELECT_SESS, (int)i };
    }
  }
  if (expIdx < 0 && st.lastSayText[0] && lastSayRect(st, showCount).contains(x, y)) {
    return UiAction{ UiAction::CHAT_REPLAY_LAST, 1 };
  }
  return UiAction{};
}

bool pageChatHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings&, ui::Rect* out) {
  if (!out || st.recState == 1) return false;

  if (st.chatDraftActive) {
    if (draftTargetRect(st).contains(x, y)) { *out = draftTargetRect(st); return true; }
    const BoardSession* target = novaBoardFind(st, st.boardTargetId);
    if (target && target->state == BoardSessState::WAIT) target = nullptr;
    if (!target) target = novaBoardDefaultTarget(st);
    ui::Rect rCancel{}, rPrimary{}, rSteer{};
    draftActionRects(st, target, &rCancel, &rPrimary, &rSteer);
    if (rCancel.contains(x, y)) { *out = rCancel; return true; }
    if (target && rPrimary.contains(x, y)) { *out = rPrimary; return true; }
    if (target && target->state == BoardSessState::RUN && rSteer.contains(x, y)) { *out = rSteer; return true; }
    return false;
  }

  char badgeTxt[24] = {};
  uint16_t bg = 0, bd = 0, fg = 0;
  bool hasWait = false;
  computeHeaderBadge(st, badgeTxt, sizeof(badgeTxt), &bg, &bd, &fg, &hasWait);
  if (hasWait && headerBadgeRect(st, badgeTxt).contains(x, y)) {
    *out = headerBadgeRect(st, badgeTxt);
    return true;
  }

  if (st.chatDelivery.kind != ChatDelivKind::NONE) {
    if (boardCount(st) > 0 && delivTargetRect(st).contains(x, y)) {
      *out = delivTargetRect(st);
      return true;
    }
    if (boardCount(st) > 1 && delivFoldRect(st).contains(x, y)) {
      *out = delivFoldRect(st);
      return true;
    }
    if (delivCardRect(st).contains(x, y)) {
      *out = delivCardRect(st);
      return true;
    }
    return false;
  }

  const uint8_t n = boardCount(st);
  const uint8_t showCount = (n <= 3) ? n : 3;
  const int expIdx = expandedIndex(st);
  for (uint8_t i = 0; i < showCount; i++) {
    const ui::Rect cr = boardCardRect(st, i, showCount, expIdx);
    if (cr.contains(x, y)) {
      *out = cr;
      return true;
    }
  }
  if (expIdx < 0 && st.lastSayText[0] && lastSayRect(st, showCount).contains(x, y)) {
    *out = lastSayRect(st, showCount);
    return true;
  }
  return false;
}

void pageChatTouchDown(int16_t, uint32_t) {}
bool pageChatScrollTouchMove(int16_t, uint32_t, const NovaState&) { return false; }
void pageChatScrollTouchUp(uint32_t, const NovaState&) {}
bool pageChatTick(uint32_t, const NovaState&) { return false; }
bool pageChatIsFlinging() { return false; }
void pageChatScrollCancel() {}
void pageChatOnExit() { pageChatScrollCancel(); }
