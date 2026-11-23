#include "ui/pages.h"
#include "ui/theme.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// 设置页：分组卡片式一页清单（针对 1.8" 368×448 穿戴屏幕降低密度、放大触控热区）。
//
// 6 个分组各自收进一张圆角卡片里，点行就地展开控件，不跳页、不换 tab。
// 滚动与阻尼回弹对齐 page_rank.cpp 物理参数（0.4 阻尼、160ms 回弹、0.94 惯性衰减），
// 展开/收起采用 150ms easeOutCubic 动画并走局部区域推送，保证 60fps 丝滑手感。

namespace {

// ---------- 尺寸（368×448，header 60 + 列表 387）----------
constexpr int16_t HDR_H          = 60;
constexpr int16_t LIST_TOP       = HDR_H + 1;
constexpr int16_t VIEW_H         = UI_H - LIST_TOP;
constexpr int16_t BACK_TAP_W     = 120;

constexpr int16_t CARD_X         = 16;
constexpr int16_t CARD_W         = UI_W - 2 * CARD_X;   // 336
constexpr int16_t CARD_R         = 16;
constexpr int16_t INNER_PAD      = 16;
constexpr int16_t TEXT_X         = CARD_X + INNER_PAD;   // 32
constexpr int16_t RIGHT_X        = CARD_X + CARD_W - INNER_PAD;  // 336
constexpr int16_t INNER_W        = RIGHT_X - TEXT_X;     // 304

constexpr int16_t GRP_H          = 38;
constexpr int16_t GRP_GAP        = 12;
constexpr int16_t ROW_H          = 62;
constexpr int16_t OPT_H          = 44;
constexpr int16_t OPT_GAP        = 8;
constexpr int16_t OPT_TOP_PAD    = 4;
constexpr int16_t OPT_BOT_PAD    = 14;
constexpr int16_t SLIDER_H       = 58;
constexpr int16_t SLIDER_BOT_PAD = 10;
constexpr int16_t SLIDER_INSET   = 14;
constexpr int16_t SW_W           = 46;
constexpr int16_t SW_H           = 26;
constexpr int16_t TAIL_H         = 52;
constexpr int16_t BAR_W          = 3;
constexpr int16_t BAR_INSET      = 10;

constexpr float    OVERSCROLL_MAX  = 28.0f;
constexpr float    OVERSCROLL_DAMP = 0.4f;
constexpr uint32_t BOUNCE_MS       = 160u;
constexpr float    FLING_DECAY     = 0.94f;
constexpr float    FLING_STOP_V    = 0.3f;
constexpr uint32_t EXPAND_ANIM_MS  = 150u;
constexpr uint32_t CONFIRM_TIMEOUT_MS = 3000u;

constexpr uint16_t COL_HDR_LINE   = rgb565(18, 26, 40);
constexpr uint16_t COL_ROW_LINE   = rgb565(22, 34, 54);
constexpr uint16_t COL_CONFIRM_BG = rgb565(46, 18, 28);
constexpr uint16_t COL_BAR_TRACK  = rgb565(12, 19, 28);
constexpr uint16_t COL_BAR_THUMB  = rgb565(56, 80, 114);

// ---------- 行 ----------
enum RowKind : uint8_t { RK_GROUP, RK_CHOICE, RK_SLIDER, RK_TOGGLE, RK_ACT };

enum RowId : uint8_t {
  R_GRP_SCR = 0, R_BRIGHT, R_SLEEP,
  R_GRP_AOD,     R_AOD, R_AOD_IDLE, R_AOD_NIGHT,
  R_GRP_SND,     R_VOLUME, R_TTS, R_QUIET, R_TEST,
  R_GRP_BAT,     R_CHGLIM, R_CHGSPD,
  R_GRP_BEH,     R_HOME, R_PWR, R_CHOREO,
  R_GRP_DEV,     R_REPROV, R_RESET,
  R_ROW_COUNT
};

struct ChoiceSet {
  const char* const* labels;
  const uint16_t*    values;
  uint8_t            n;
};

const char* const kSleepLbl[] = { "15 秒", "30 秒", "1 分钟", "2 分钟" };
const uint16_t    kSleepVal[] = { 15, 30, 60, 120 };
const char* const kAodLbl[]   = { "关", "仅插电", "常开" };
const uint16_t    kAodVal[]   = { 0, 1, 2 };
const char* const kIdleLbl[]  = { "1 分钟", "2 分钟", "5 分钟" };
const uint16_t    kIdleVal[]  = { 60, 120, 300 };
const char* const kNightLbl[] = { "暗一档", "暗两档", "跟随亮度" };
const uint16_t    kNightVal[] = { 0, 1, 2 };
const char* const kLimLbl[]   = { "80%", "90%", "100%" };
const uint16_t    kLimVal[]   = { 80, 90, 100 };
const char* const kSpdLbl[]   = { "温和", "自动", "快速" };
const uint16_t    kSpdVal[]   = { 0, 1, 2 };
const char* const kHomeLbl[]  = { "宠物", "订阅", "榜单" };
const uint16_t    kHomeVal[]  = { 0, 1, 2 };
const char* const kPwrLbl[]   = { "自适应", "长续航", "灵动" };
const uint16_t    kPwrVal[]   = { 0, 1, 2 };

constexpr ChoiceSet kSets[] = {
  { kSleepLbl, kSleepVal, 4 },   // 0 息屏
  { kAodLbl,   kAodVal,   3 },   // 1 熄屏显示
  { kIdleLbl,  kIdleVal,  3 },   // 2 多久切表盘
  { kNightLbl, kNightVal, 3 },   // 3 亮度保护
  { kLimLbl,   kLimVal,   3 },   // 4 充电上限
  { kSpdLbl,   kSpdVal,   3 },   // 5 充电速度
  { kHomeLbl,  kHomeVal,  3 },   // 6 默认首页
  { kPwrLbl,   kPwrVal,   3 },   // 7 功耗档
};

struct RowDef {
  uint8_t     kind;
  uint8_t     id;
  const char* label;
  uint8_t     set;     // RK_CHOICE 用
};

constexpr RowDef kRows[] = {
  { RK_GROUP,  R_GRP_SCR,   "屏幕",          0 },
  { RK_SLIDER, R_BRIGHT,    "亮度",          0 },
  { RK_CHOICE, R_SLEEP,     "息屏",          0 },

  { RK_GROUP,  R_GRP_AOD,   "熄屏显示",      0 },
  { RK_CHOICE, R_AOD,       "什么时候开",    1 },
  { RK_CHOICE, R_AOD_IDLE,  "多久切表盘",    2 },
  { RK_CHOICE, R_AOD_NIGHT, "亮度保护",      3 },

  { RK_GROUP,  R_GRP_SND,   "声音",          0 },
  { RK_SLIDER, R_VOLUME,    "音量",          0 },
  { RK_TOGGLE, R_TTS,       "语音播报",      0 },
  { RK_TOGGLE, R_QUIET,     "夜间静音 23-08", 0 },
  { RK_ACT,    R_TEST,      "试听语音",      0 },

  { RK_GROUP,  R_GRP_BAT,   "电池",          0 },
  { RK_CHOICE, R_CHGLIM,    "充电上限",      4 },
  { RK_CHOICE, R_CHGSPD,    "充电速度",      5 },

  { RK_GROUP,  R_GRP_BEH,   "行为",          0 },
  { RK_CHOICE, R_HOME,      "默认首页",      6 },
  { RK_CHOICE, R_PWR,       "功耗档",        7 },
  { RK_TOGGLE, R_CHOREO,    "AI 编排",       0 },

  { RK_GROUP,  R_GRP_DEV,   "设备",          0 },
  { RK_ACT,    R_REPROV,    "重新配网",      0 },
  { RK_ACT,    R_RESET,     "恢复默认",      0 },
};
constexpr uint8_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

// ---------- 滚动与动画状态 ----------
struct TouchSample {
  int16_t  y;
  uint32_t ms;
};

float       s_scrollY           = 0.0f;
bool        s_scrollTouchActive = false;
int16_t     s_lastTouchY        = 0;
TouchSample s_samples[3]        = {};
uint8_t     s_sampleCount       = 0;

bool        s_flinging          = false;
float       s_flingVel          = 0.0f;   // 每 16.67ms 帧的 scrollY 增量
uint32_t    s_lastTickMs        = 0;

bool        s_bouncing          = false;
float       s_bounceFrom        = 0.0f;
float       s_bounceTo          = 0.0f;
uint32_t    s_bounceStartMs     = 0;

// 展开 / 收起平滑动画
int8_t      s_openRow           = -1;
float       s_openFrac          = 0.0f;
float       s_openFrom          = 0.0f;
int8_t      s_closeRow          = -1;
float       s_closeFrac         = 0.0f;
float       s_closeFrom         = 0.0f;
uint32_t    s_animStartMs       = 0;
bool        s_animActive        = false;
bool        s_scrollTween       = false;
float       s_scrollFrom        = 0.0f;
float       s_scrollTo          = 0.0f;

// 滑块 / 危险操作确认状态
int8_t      s_dragRow           = -1;
int8_t      s_confirmRow        = -1;
uint32_t    s_confirmAtMs       = 0;

int8_t findRowById(uint8_t id) {
  for (uint8_t i = 0; i < kRowCount; i++) {
    if (kRows[i].id == id) return (int8_t)i;
  }
  return -1;
}

float easeOutCubic(float u) {
  if (u <= 0.0f) return 0.0f;
  if (u >= 1.0f) return 1.0f;
  float inv = 1.0f - u;
  return 1.0f - inv * inv * inv;
}

// ---------- 取值 / 回写 ----------
uint16_t rowValue(const NovaSettings& cfg, uint8_t id) {
  switch (id) {
    case R_BRIGHT:    return cfg.brightPct;
    case R_SLEEP:     return cfg.batSleepSec;
    case R_AOD:       return cfg.aodMode;
    case R_AOD_IDLE:  return cfg.aodIdleSec;
    case R_AOD_NIGHT: return cfg.aodNightCap;
    case R_VOLUME:    return cfg.soundPct;
    case R_TTS:       return cfg.ttsEnabled ? 1 : 0;
    case R_QUIET:     return cfg.quietHours ? 1 : 0;
    case R_CHGLIM:    return cfg.chargeLimitPct;
    case R_CHGSPD:    return (uint16_t)cfg.chargeSpeed;
    case R_HOME:      return (uint16_t)cfg.homePage;
    case R_PWR:       return (uint16_t)cfg.powerProfile;
    case R_CHOREO:    return cfg.choreoEnabled ? 1 : 0;
    default:          return 0;
  }
}

UiAction::Kind rowActionKind(uint8_t id) {
  switch (id) {
    case R_BRIGHT:    return UiAction::SET_BRIGHT;
    case R_SLEEP:     return UiAction::SET_BATSLEEP;
    case R_AOD:       return UiAction::SET_AOD_MODE;
    case R_AOD_IDLE:  return UiAction::SET_AOD_IDLE;
    case R_AOD_NIGHT: return UiAction::SET_AOD_NIGHT;
    case R_VOLUME:    return UiAction::SET_SOUND;
    case R_TTS:       return UiAction::SET_TTS;
    case R_QUIET:     return UiAction::SET_QUIET;
    case R_CHGLIM:    return UiAction::SET_CHGLIMIT;
    case R_CHGSPD:    return UiAction::SET_CHGSPEED;
    case R_HOME:      return UiAction::SET_HOME;
    case R_PWR:       return UiAction::SET_POWER;
    case R_CHOREO:    return UiAction::SET_CHOREO;
    case R_TEST:      return UiAction::TEST_SOUND;
    case R_REPROV:    return UiAction::REPROVISION;
    case R_RESET:     return UiAction::RESTORE_DEFAULTS;
    default:          return UiAction::NONE;
  }
}

const char* choiceLabel(const ChoiceSet& cs, uint16_t v) {
  for (uint8_t i = 0; i < cs.n; i++) {
    if (cs.values[i] == v) return cs.labels[i];
  }
  return "--";
}

void valueText(const NovaSettings& cfg, uint8_t id, char* buf, size_t n) {
  buf[0] = '\0';
  if (id == R_BRIGHT) {
    snprintf(buf, n, "%u%%", (unsigned)cfg.brightPct);
    return;
  }
  if (id == R_VOLUME) {
    snprintf(buf, n, "%u%%", (unsigned)cfg.soundPct);
    return;
  }
  const int8_t idx = findRowById(id);
  const uint8_t setIdx = (idx >= 0) ? kRows[idx].set : 0;
  snprintf(buf, n, "%s", choiceLabel(kSets[setIdx], rowValue(cfg, id)));
}

// ---------- 布局计算 ----------
uint8_t choiceCols(uint8_t n) {
  return (n >= 4) ? 2 : n;
}

uint8_t choiceRows(uint8_t n) {
  const uint8_t cols = choiceCols(n);
  return (uint8_t)((n + cols - 1) / cols);
}

ui::Rect choicePillRect(int16_t rowTopY, uint8_t setIdx, uint8_t optIdx) {
  const uint8_t n = kSets[setIdx].n;
  const uint8_t cols = choiceCols(n);
  const int16_t w = (int16_t)((INNER_W - OPT_GAP * (cols - 1)) / cols);
  const uint8_t col = (uint8_t)(optIdx % cols);
  const uint8_t row = (uint8_t)(optIdx / cols);
  const int16_t x = (int16_t)(TEXT_X + col * (w + OPT_GAP));
  const int16_t y = (int16_t)(rowTopY + ROW_H + OPT_TOP_PAD + row * (OPT_H + OPT_GAP));
  return ui::Rect{ x, y, w, OPT_H };
}

int16_t fullExtraH(uint8_t i) {
  const RowDef& r = kRows[i];
  if (r.kind == RK_SLIDER) {
    return (int16_t)(SLIDER_H + SLIDER_BOT_PAD);
  }
  if (r.kind == RK_CHOICE) {
    const uint8_t rows = choiceRows(kSets[r.set].n);
    const int16_t optArea = (int16_t)(rows * OPT_H + (rows - 1) * OPT_GAP);
    return (int16_t)(OPT_TOP_PAD + optArea + OPT_BOT_PAD);
  }
  return 0;
}

float rowExpandFrac(uint8_t i) {
  if ((int8_t)i == s_openRow) return s_openFrac;
  if ((int8_t)i == s_closeRow) return s_closeFrac;
  return 0.0f;
}

int16_t rowH(uint8_t i) {
  const RowDef& r = kRows[i];
  if (r.kind == RK_GROUP) {
    return (i == 0) ? GRP_H : (int16_t)(GRP_H + GRP_GAP);
  }
  const float frac = rowExpandFrac(i);
  if (frac <= 0.0f) return ROW_H;
  return (int16_t)(ROW_H + (int16_t)lroundf((float)fullExtraH(i) * frac));
}

// 目标终态下的行高（用于计算展开动画目标 scrollY）。
int16_t targetRowH(uint8_t i) {
  const RowDef& r = kRows[i];
  if (r.kind == RK_GROUP) {
    return (i == 0) ? GRP_H : (int16_t)(GRP_H + GRP_GAP);
  }
  if ((int8_t)i == s_openRow) {
    return (int16_t)(ROW_H + fullExtraH(i));
  }
  return ROW_H;
}

int16_t contentH() {
  int16_t h = TAIL_H;
  for (uint8_t i = 0; i < kRowCount; i++) h = (int16_t)(h + rowH(i));
  return h;
}

int16_t targetContentH() {
  int16_t h = TAIL_H;
  for (uint8_t i = 0; i < kRowCount; i++) h = (int16_t)(h + targetRowH(i));
  return h;
}

int16_t rowTop(uint8_t i) {
  int16_t y = 0;
  for (uint8_t k = 0; k < i; k++) y = (int16_t)(y + rowH(k));
  return y;
}

int16_t targetRowTop(uint8_t i) {
  int16_t y = 0;
  for (uint8_t k = 0; k < i; k++) y = (int16_t)(y + targetRowH(k));
  return y;
}

float maxScrollF() {
  const int16_t h = contentH();
  return (h > VIEW_H) ? (float)(h - VIEW_H) : 0.0f;
}

float targetMaxScrollF() {
  const int16_t h = targetContentH();
  return (h > VIEW_H) ? (float)(h - VIEW_H) : 0.0f;
}

void clampScroll() {
  const float m = maxScrollF();
  if (s_scrollY < 0.0f) s_scrollY = 0.0f;
  if (s_scrollY > m) s_scrollY = m;
}

void startBounce(float target, uint32_t ms) {
  s_flinging = false;
  s_flingVel = 0.0f;
  s_bouncing = true;
  s_bounceFrom = s_scrollY;
  s_bounceTo = target;
  s_bounceStartMs = ms;
}

float clampTargetScroll(float y) {
  const float m = targetMaxScrollF();
  if (y < 0.0f) return 0.0f;
  if (y > m) return m;
  return y;
}

float visibleScrollForTargetRow(int8_t idx, float curTargetScroll) {
  float sy = clampTargetScroll(curTargetScroll);
  if (idx < 0 || (uint8_t)idx >= kRowCount) return sy;
  const float top = (float)targetRowTop((uint8_t)idx);
  const float bot = top + (float)targetRowH((uint8_t)idx);
  if (top < sy) {
    sy = top;
  } else if (bot > sy + (float)VIEW_H) {
    sy = bot - (float)VIEW_H;
  }
  return clampTargetScroll(sy);
}

// ---------- 命中 ----------
struct Hit {
  int8_t  row;    // -1 = 没命中
  uint8_t zone;   // 0 = 行主体，1 = 选项胶囊，2 = 滑块，3 = 展开区留白
  uint8_t opt;    // zone==1 时的选项下标
};

Hit hitTest(int16_t x, int16_t y) {
  Hit h{ -1, 0, 0 };
  if (y < LIST_TOP) return h;

  const int16_t scrollPx = (int16_t)lroundf(s_scrollY);
  const int16_t contentY = (int16_t)(y - LIST_TOP + scrollPx);
  int16_t acc = 0;
  for (uint8_t i = 0; i < kRowCount; i++) {
    const int16_t rh = rowH(i);
    if (contentY >= acc && contentY < acc + rh) {
      h.row = (int8_t)i;
      const RowDef& r = kRows[i];
      if (r.kind == RK_GROUP) return h;
      if (x < CARD_X || x >= CARD_X + CARD_W) { h.row = -1; return h; }

      const int16_t rel = (int16_t)(contentY - acc);
      if ((int8_t)i == s_openRow && rel >= ROW_H) {
        if (r.kind == RK_SLIDER) {
          h.zone = 2;
          return h;
        }
        if (s_openFrac >= 0.90f) {
          const ChoiceSet& cs = kSets[r.set];
          for (uint8_t o = 0; o < cs.n; o++) {
            const ui::Rect pr = choicePillRect(0, r.set, o);
            if (x >= pr.x && x < pr.x + pr.w && rel >= pr.y && rel < pr.y + pr.h) {
              h.zone = 1;
              h.opt = o;
              return h;
            }
          }
        }
        h.zone = 3;
        return h;
      }
      return h;
    }
    acc = (int16_t)(acc + rh);
  }
  return h;
}

// ---------- 绘制 ----------
void drawHeader(NovaCanvas& c) {
  c.fillRect(0, 0, UI_W, HDR_H, ui::COL_BG);
  c.drawLine(31, 22, 23, 30, ui::COL_ACCENT);
  c.drawLine(23, 30, 31, 38, ui::COL_ACCENT);
  c.drawLine(32, 22, 24, 30, ui::COL_ACCENT);
  c.drawLine(24, 30, 32, 38, ui::COL_ACCENT);
  ui::drawText(c, ui::fontCjk16(), 44, 37, "设置", ui::COL_TEXT, ui::COL_BG);
  c.drawFastHLine(0, HDR_H, UI_W, COL_HDR_LINE);
}

void drawTextVC(NovaCanvas& c, const AaFont* f, int16_t x, int16_t top, int16_t h,
                const char* s, uint16_t fg, uint16_t bg) {
  if (!s || !s[0]) return;
  ui::TextMetrics m = ui::measureText(c, f, s);
  int16_t ty = (int16_t)(top + (h - (int16_t)m.h) / 2 - m.y1);
  ui::drawText(c, f, x, ty, s, fg, bg);
}

void drawTextVCRight(NovaCanvas& c, const AaFont* f, int16_t rightX, int16_t top, int16_t h,
                     const char* s, uint16_t fg, uint16_t bg) {
  if (!s || !s[0]) return;
  ui::TextMetrics m = ui::measureText(c, f, s);
  int16_t tx = (int16_t)(rightX - (int16_t)m.w - m.x1);
  int16_t ty = (int16_t)(top + (h - (int16_t)m.h) / 2 - m.y1);
  ui::drawText(c, f, tx, ty, s, fg, bg);
}

void drawPlayButton(NovaCanvas& c, int16_t cx, int16_t cy, bool active) {
  const uint16_t ring = active ? ui::COL_ACCENT : rgb565(46, 68, 98);
  ui::aaDrawCircle(c, cx, cy, 18, ring);
  ui::aaDrawCircle(c, cx, cy, 17, ring);
  const int16_t x0 = cx - 4;
  c.fillTriangle(x0, cy - 6, x0, cy + 6, x0 + 10, cy, ui::COL_ACCENT);
}

void drawChevronRight(NovaCanvas& c, int16_t tipX, int16_t cy, uint16_t col) {
  c.drawLine(tipX - 6, cy - 5, tipX - 1, cy, col);
  c.drawLine(tipX - 1, cy, tipX - 6, cy + 5, col);
  c.drawLine(tipX - 7, cy - 5, tipX - 2, cy, col);
  c.drawLine(tipX - 2, cy, tipX - 7, cy + 5, col);
}

void drawChoiceRow(NovaCanvas& c, int16_t top, const NovaSettings& cfg, const RowDef& r) {
  const ChoiceSet& cs = kSets[r.set];
  const uint16_t cur = rowValue(cfg, r.id);
  for (uint8_t i = 0; i < cs.n; i++) {
    const ui::Rect pr = choicePillRect(top, r.set, i);
    const bool on = (cs.values[i] == cur);
    const uint16_t bg = on ? ui::COL_ACCENT : ui::COL_CARD_HI;
    const uint16_t bd = on ? ui::COL_ACCENT : ui::COL_BORDER;
    const uint16_t fg = on ? ui::COL_BG : ui::COL_TEXT;
    ui::aaFillRoundRect(c, pr.x, pr.y, pr.w, pr.h, 11, bg, bd);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), pr, cs.labels[i], fg, bg);
  }
}

void drawSliderRow(NovaCanvas& c, int16_t top, const NovaSettings& cfg, const RowDef& r) {
  const bool bright = (r.id == R_BRIGHT);
  const uint16_t lo = bright ? 20 : 0;
  const uint16_t hi = 100;
  uint16_t v = rowValue(cfg, r.id);
  if (v < lo) v = lo;
  if (v > hi) v = hi;

  const int16_t x0 = (int16_t)(TEXT_X + SLIDER_INSET);
  const int16_t x1 = (int16_t)(RIGHT_X - SLIDER_INSET);
  const int16_t cy = (int16_t)(top + ROW_H + SLIDER_H / 2);
  const int16_t trackH = 8;

  ui::aaFillRoundRect(c, x0, (int16_t)(cy - trackH / 2), (int16_t)(x1 - x0), trackH,
                      trackH / 2, ui::COL_TRACK, ui::COL_TRACK);
  const int16_t span = (int16_t)(x1 - x0);
  const int16_t knobX = (int16_t)(x0 + (int32_t)span * (v - lo) / (hi - lo));
  if (knobX > x0) {
    int16_t fw = (int16_t)(knobX - x0);
    if (fw < trackH) fw = trackH;
    ui::aaFillRoundRect(c, x0, (int16_t)(cy - trackH / 2), fw, trackH,
                        trackH / 2, ui::COL_ACCENT, ui::COL_ACCENT);
  }
  ui::aaDrawCircle(c, knobX, cy, 15, rgb565(30, 56, 86));
  ui::aaFillCircle(c, knobX, cy, 12, ui::COL_TEXT);
  ui::aaFillCircle(c, knobX, cy, 4, ui::COL_ACCENT);
}

void drawItemRow(NovaCanvas& c, int16_t top, const NovaSettings& cfg, uint8_t idx, bool lastInGroup) {
  const RowDef& r = kRows[idx];
  const bool confirm = ((int8_t)idx == s_confirmRow);
  uint16_t rowBg = ui::COL_CARD;
  uint16_t nameCol = ui::COL_TEXT;
  if (r.id == R_REPROV) nameCol = ui::COL_AMBER;
  if (r.id == R_RESET)  nameCol = ui::COL_DIM;
  if (confirm) {
    rowBg = COL_CONFIRM_BG;
    nameCol = ui::COL_RED;
    ui::aaFillRoundRect(c, (int16_t)(CARD_X + 4), (int16_t)(top + 4),
                        (int16_t)(CARD_W - 8), (int16_t)(ROW_H - 8), 12,
                        COL_CONFIRM_BG, ui::COL_RED);
  }

  drawTextVC(c, ui::fontCjk16(), TEXT_X, top, ROW_H, r.label, nameCol, rowBg);

  char val[24];
  switch (r.kind) {
    case RK_CHOICE:
    case RK_SLIDER:
      valueText(cfg, r.id, val, sizeof(val));
      drawTextVCRight(c, ui::fontCjk16(), RIGHT_X, top, ROW_H, val, ui::COL_ACCENT, rowBg);
      break;
    case RK_TOGGLE: {
      const int16_t sy = (int16_t)(top + (ROW_H - SW_H) / 2);
      ui::drawSwitch(c, ui::Rect{ (int16_t)(RIGHT_X - SW_W), sy, SW_W, SW_H },
                     rowValue(cfg, r.id) != 0, ui::COL_ACCENT);
      break;
    }
    case RK_ACT:
      if (r.id == R_TEST) {
        drawPlayButton(c, (int16_t)(RIGHT_X - 18), (int16_t)(top + ROW_H / 2), false);
      } else if (confirm) {
        drawTextVCRight(c, ui::fontCjk14(), (int16_t)(RIGHT_X - 6), top, ROW_H,
                        "再点一次", ui::COL_RED, rowBg);
      } else {
        drawChevronRight(c, RIGHT_X, (int16_t)(top + ROW_H / 2), ui::COL_MUTED);
      }
      break;
    default:
      break;
  }

  // 展开区内容仅在抽屉接近到位时绘制，防止无裁剪画布下越出卡片底边。
  if (rowExpandFrac(idx) >= 0.90f) {
    if (r.kind == RK_SLIDER) {
      drawSliderRow(c, top, cfg, r);
    } else if (r.kind == RK_CHOICE) {
      drawChoiceRow(c, top, cfg, r);
    }
  }

  if (!lastInGroup) {
    c.drawFastHLine(TEXT_X, (int16_t)(top + rowH(idx) - 1), INNER_W, COL_ROW_LINE);
  }
}

void drawScrollbar(NovaCanvas& c) {
  const int16_t total = contentH();
  if (total <= VIEW_H) return;
  const int16_t trackY = (int16_t)(LIST_TOP + BAR_INSET);
  const int16_t trackH = (int16_t)(VIEW_H - 2 * BAR_INSET);
  int16_t thumbH = (int16_t)((int32_t)trackH * VIEW_H / total);
  if (thumbH < 28) thumbH = 28;
  const float m = maxScrollF();
  float clamped = s_scrollY;
  if (clamped < 0.0f) clamped = 0.0f;
  if (clamped > m) clamped = m;
  const int16_t thumbY = (m > 0.0f)
      ? (int16_t)(trackY + (int32_t)lroundf((float)(trackH - thumbH) * (clamped / m)))
      : trackY;
  c.fillRect(UI_W - 5, trackY, BAR_W, trackH, COL_BAR_TRACK);
  c.fillRect(UI_W - 5, thumbY, BAR_W, thumbH, COL_BAR_THUMB);
}

void drawList(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg) {
  c.fillRect(0, LIST_TOP, UI_W, VIEW_H, ui::COL_BG);
  const int16_t scrollPx = (int16_t)lroundf(s_scrollY);
  int16_t y = (int16_t)(LIST_TOP - scrollPx);

  uint8_t i = 0;
  while (i < kRowCount) {
    if (kRows[i].kind != RK_GROUP) {
      y = (int16_t)(y + rowH(i));
      i++;
      continue;
    }

    const int16_t gh = rowH(i);
    const int16_t labelTop = (i == 0) ? y : (int16_t)(y + GRP_GAP);
    if (y + gh > LIST_TOP - 1 && y < UI_H) {
      drawTextVC(c, ui::fontCjk14(), (int16_t)(CARD_X + 8), labelTop, GRP_H,
                 kRows[i].label, ui::COL_MUTED, ui::COL_BG);
    }
    y = (int16_t)(y + gh);

    const uint8_t firstItem = (uint8_t)(i + 1);
    uint8_t endItem = firstItem;
    int16_t cardH = 0;
    while (endItem < kRowCount && kRows[endItem].kind != RK_GROUP) {
      cardH = (int16_t)(cardH + rowH(endItem));
      endItem++;
    }

    if (cardH > 0 && y + cardH > LIST_TOP - 1 && y < UI_H) {
      ui::drawCard(c, CARD_X, y, CARD_W, cardH, ui::COL_CARD, ui::COL_BORDER, CARD_R);
      int16_t iy = y;
      for (uint8_t j = firstItem; j < endItem; j++) {
        const int16_t rh = rowH(j);
        if (iy + rh > LIST_TOP - 1 && iy < UI_H) {
          drawItemRow(c, iy, cfg, j, j + 1 == endItem);
        }
        iy = (int16_t)(iy + rh);
      }
    }
    y = (int16_t)(y + cardH);
    i = endItem;
  }

  // 尾巴：固件版本 + 设备号，滚到底才看得到
  if (y + TAIL_H > LIST_TOP && y < UI_H) {
    char tail[48];
    snprintf(tail, sizeof(tail), "v%s · Nova-%s", NOVA_FW_VER, st.deviceId);
    ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, (int16_t)(y + 30), tail,
                         ui::COL_MUTED, ui::COL_BG);
  }
  drawScrollbar(c);
}

void applySliderX(int16_t x) {
  if (s_dragRow < 0) return;
  const RowDef& r = kRows[s_dragRow];
  const bool bright = (r.id == R_BRIGHT);
  const int16_t lo = bright ? 20 : 0;
  const int16_t hi = 100;
  const int16_t x0 = (int16_t)(TEXT_X + SLIDER_INSET);
  const int16_t x1 = (int16_t)(RIGHT_X - SLIDER_INSET);
  int16_t t = (int16_t)(x - x0);
  if (t < 0) t = 0;
  if (t > x1 - x0) t = (int16_t)(x1 - x0);
  const int32_t raw = (int32_t)lo + (int32_t)(hi - lo) * t / (x1 - x0);
  uint16_t v = (uint16_t)((raw + 2) / 5 * 5);   // 吸附到 5% 步进
  if (v < lo) v = lo;
  if (v > hi) v = hi;

  NovaSettings& cfg = settingsMut();
  if (r.id == R_BRIGHT) cfg.brightPct = (uint8_t)v;
  else                  cfg.soundPct = (uint8_t)v;
}

void resetTransientState(bool resetScroll) {
  s_openRow = -1;
  s_openFrac = 0.0f;
  s_openFrom = 0.0f;
  s_closeRow = -1;
  s_closeFrac = 0.0f;
  s_closeFrom = 0.0f;
  s_animActive = false;
  s_scrollTween = false;
  s_dragRow = -1;
  s_confirmRow = -1;
  s_scrollTouchActive = false;
  s_flinging = false;
  s_bouncing = false;
  s_flingVel = 0.0f;
  s_sampleCount = 0;
  if (resetScroll) s_scrollY = 0.0f;
}

}  // namespace

// ================= 对外接口 =================

void pageSettingsSetTab(uint8_t tab) {
  static const uint8_t kTarget[4] = { R_GRP_SCR, R_GRP_AOD, R_GRP_SND, R_GRP_BAT };
  resetTransientState(false);
  const int8_t idx = (tab < 4) ? findRowById(kTarget[tab]) : 0;
  if (idx > 0) {
    s_scrollY = (float)(rowTop((uint8_t)idx) + GRP_GAP);
  } else {
    s_scrollY = 0.0f;
  }
  clampScroll();
}

void pageSettingsOnEnter() {
  resetTransientState(true);
}

void pageSettingsCancelDrag() {
  s_dragRow = -1;
  s_scrollTouchActive = false;
  s_sampleCount = 0;
}

#ifdef NOVA_HOST_SIM
void pageSettingsDebugOpen(uint8_t id) {
  const int8_t idx = findRowById(id);
  if (idx >= 0) {
    s_openRow = idx;
    s_openFrac = 1.0f;
    s_closeRow = -1;
    s_closeFrac = 0.0f;
    s_animActive = false;
    s_scrollTween = false;
    s_scrollY = visibleScrollForTargetRow(idx, s_scrollY);
  }
}

void pageSettingsDebugConfirm(uint8_t id) {
  const int8_t idx = findRowById(id);
  if (idx < 0) return;
  s_confirmRow = idx;
  s_confirmAtMs = 0;
  const int16_t top = rowTop((uint8_t)idx);
  s_scrollY = (float)(top - VIEW_H / 2);
  clampScroll();
}
#endif

bool pageSettingsIsAnimating() {
  return s_flinging || s_bouncing || s_animActive;
}

ui::Rect pageSettingsListRect() {
  return { 0, LIST_TOP, UI_W, VIEW_H };
}

bool pageSettingsTick(uint32_t nowMs) {
  bool dirty = false;

  if (s_confirmRow >= 0 && nowMs > s_confirmAtMs &&
      (nowMs - s_confirmAtMs) >= CONFIRM_TIMEOUT_MS) {
    s_confirmRow = -1;
    dirty = true;
  }

  // 1. 展开 / 收起动画
  if (s_animActive) {
    const uint32_t elapsed = (nowMs > s_animStartMs) ? (nowMs - s_animStartMs) : 0u;
    if (elapsed >= EXPAND_ANIM_MS) {
      s_openFrac = (s_openRow >= 0) ? 1.0f : 0.0f;
      s_closeRow = -1;
      s_closeFrac = 0.0f;
      s_animActive = false;
      if (s_scrollTween) {
        s_scrollY = s_scrollTo;
        s_scrollTween = false;
      }
      clampScroll();
    } else {
      const float e = easeOutCubic((float)elapsed / (float)EXPAND_ANIM_MS);
      if (s_openRow >= 0) {
        s_openFrac = s_openFrom + (1.0f - s_openFrom) * e;
      }
      if (s_closeRow >= 0) {
        s_closeFrac = s_closeFrom * (1.0f - e);
      }
      if (s_scrollTween) {
        s_scrollY = s_scrollFrom + (s_scrollTo - s_scrollFrom) * e;
      }
    }
    dirty = true;
  }

  // 2. 越界回弹（与 page_rank.cpp 同一套 easeOutCubic 参数）
  if (!s_scrollTouchActive) {
    const float maxS = maxScrollF();
    if (s_bouncing) {
      int32_t elapsed = (int32_t)(nowMs - s_bounceStartMs);
      if (elapsed < 0) elapsed = 0;
      if ((uint32_t)elapsed >= BOUNCE_MS) {
        s_scrollY = s_bounceTo;
        s_bouncing = false;
      } else {
        const float e = easeOutCubic((float)elapsed / (float)BOUNCE_MS);
        s_scrollY = s_bounceFrom + (s_bounceTo - s_bounceFrom) * e;
      }
      dirty = true;
    } else if (s_flinging) {
      int32_t rawDt = (int32_t)(nowMs - s_lastTickMs);
      s_lastTickMs = nowMs;
      if (rawDt < 1) rawDt = 1;
      if (rawDt > 50) rawDt = 50;
      const float frames = (float)rawDt / 16.67f;

      const bool inOver = (s_scrollY < 0.0f || s_scrollY > maxS);
      if (!inOver) {
        s_scrollY += s_flingVel * frames;
        s_flingVel *= powf(FLING_DECAY, frames);

        if (s_scrollY < 0.0f) {
          s_scrollY *= OVERSCROLL_DAMP;
          if (s_scrollY <= -OVERSCROLL_MAX) {
            s_scrollY = -OVERSCROLL_MAX;
            startBounce(0.0f, nowMs);
          }
        } else if (s_scrollY > maxS) {
          s_scrollY = maxS + (s_scrollY - maxS) * OVERSCROLL_DAMP;
          if (s_scrollY >= maxS + OVERSCROLL_MAX) {
            s_scrollY = maxS + OVERSCROLL_MAX;
            startBounce(maxS, nowMs);
          }
        } else if (fabsf(s_flingVel) < FLING_STOP_V) {
          s_flinging = false;
          s_flingVel = 0.0f;
        }
      } else {
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
      dirty = true;
    }
  }

  return dirty;
}

void pageSettingsDrawList(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg) {
  if (!s_scrollTouchActive && !s_flinging && !s_bouncing && !s_scrollTween) {
    clampScroll();
  }
  drawList(c, st, cfg);
  drawHeader(c);
}

void pageSettingsDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg) {
  c.fillScreen(ui::COL_BG);
  pageSettingsDrawList(c, st, cfg);
}

UiAction pageSettingsTap(int16_t x, int16_t y, const NovaSettings& cfg) {
  s_scrollTouchActive = false;
  s_sampleCount = 0;

  // header：左上返回（覆盖 < 与「设置」标题）
  if (y <= HDR_H) {
    if (x < BACK_TAP_W) {
      resetTransientState(false);
      return UiAction{ UiAction::GO_PAGE, (int)UiPage::DETAIL };
    }
    return UiAction{};
  }

  const Hit h = hitTest(x, y);
  if (h.row < 0) return UiAction{};

  const RowDef& r = kRows[h.row];
  if (r.kind == RK_GROUP) return UiAction{};

  // 点别的行 → 取消待确认
  if (s_confirmRow >= 0 && s_confirmRow != h.row) s_confirmRow = -1;

  // --- 展开区留白：不误折叠 ---
  if (h.zone == 3) return UiAction{};

  // --- 展开区的选项 ---
  if (h.zone == 1) {
    const ChoiceSet& cs = kSets[r.set];
    s_closeRow = s_openRow;
    s_closeFrom = s_openFrac;
    s_closeFrac = s_openFrac;
    s_openRow = -1;
    s_openFrac = 0.0f;
    s_animStartMs = millis();
    s_animActive = (s_closeRow >= 0);
    const float maxAfter = targetMaxScrollF();
    if (s_scrollY > maxAfter) {
      s_scrollTween = true;
      s_scrollFrom = s_scrollY;
      s_scrollTo = maxAfter;
    } else {
      s_scrollTween = false;
    }
    return UiAction{ rowActionKind(r.id), (int)cs.values[h.opt] };
  }

  // --- 滑块：按下就交给 touch 处理，tap 不做事 ---
  if (h.zone == 2) return UiAction{};

  // --- 行主体 ---
  switch (r.kind) {
    case RK_CHOICE:
    case RK_SLIDER: {
      const int8_t prevOpen = s_openRow;
      const float  prevFrac = s_openFrac;
      if (prevOpen == h.row) {
        s_closeRow = prevOpen;
        s_closeFrom = prevFrac;
        s_closeFrac = prevFrac;
        s_openRow = -1;
        s_openFrac = 0.0f;
        s_animStartMs = millis();
        s_animActive = true;
        const float maxAfter = targetMaxScrollF();
        if (s_scrollY > maxAfter) {
          s_scrollTween = true;
          s_scrollFrom = s_scrollY;
          s_scrollTo = maxAfter;
        } else {
          s_scrollTween = false;
        }
      } else {
        float desiredScroll = s_scrollY;
        // 若此前展开的行在当前点击行上方，收起它会让下方行上移；
        // 用目标 scrollY 同步回退这段高度差，让被点行留在原处不跳动。
        if (prevOpen >= 0 && prevOpen < h.row) {
          desiredScroll -= (float)fullExtraH((uint8_t)prevOpen) * prevFrac;
        }
        s_closeRow = prevOpen;
        s_closeFrom = prevFrac;
        s_closeFrac = prevFrac;
        s_openRow = h.row;
        s_openFrom = (h.row == s_closeRow) ? s_closeFrac : 0.0f;
        s_openFrac = s_openFrom;
        if (s_closeRow == h.row) {
          s_closeRow = -1;
          s_closeFrac = 0.0f;
        }
        s_animStartMs = millis();
        s_animActive = true;
        const float targetY = visibleScrollForTargetRow(s_openRow, desiredScroll);
        if (fabsf(targetY - s_scrollY) >= 0.5f) {
          s_scrollTween = true;
          s_scrollFrom = s_scrollY;
          s_scrollTo = targetY;
        } else {
          s_scrollTween = false;
        }
      }
      // value=2：只重绘并播轻提示音，不走 FADE 转场。
      return UiAction{ UiAction::SETTINGS_REDRAW, 2 };
    }

    case RK_TOGGLE:
      return UiAction{ rowActionKind(r.id), rowValue(cfg, r.id) ? 0 : 1 };

    case RK_ACT: {
      const UiAction::Kind kind = rowActionKind(r.id);
      const bool dangerous = (kind == UiAction::REPROVISION || kind == UiAction::RESTORE_DEFAULTS);
      if (dangerous) {
        if (s_confirmRow == h.row) {
          s_confirmRow = -1;
          return UiAction{ kind, 0 };
        }
        s_confirmRow = h.row;
        s_confirmAtMs = millis();
        return UiAction{ UiAction::SETTINGS_REDRAW, 2 };
      }
      return UiAction{ kind, 0 };
    }
    default:
      return UiAction{};
  }
}

bool pageSettingsHitRect(int16_t x, int16_t y, const NovaState&, const NovaSettings&,
                         ui::Rect* out) {
  if (!out) return false;
  if (y <= HDR_H) return false;
  const Hit h = hitTest(x, y);
  if (h.row < 0) return false;
  if (kRows[h.row].kind == RK_GROUP) return false;
  if (h.zone == 2 || h.zone == 3) return false;

  const int16_t scrollPx = (int16_t)lroundf(s_scrollY);
  ui::Rect r{ 0, 0, 0, 0 };
  if (h.zone == 1) {
    const int16_t rTop = (int16_t)(LIST_TOP - scrollPx + rowTop((uint8_t)h.row));
    r = choicePillRect(rTop, kRows[h.row].set, h.opt);
  } else {
    const int16_t rTop = (int16_t)(LIST_TOP - scrollPx + rowTop((uint8_t)h.row));
    r = ui::Rect{ (int16_t)(CARD_X + 2), (int16_t)(rTop + 2),
                  (int16_t)(CARD_W - 4), (int16_t)(ROW_H - 4) };
  }

  int16_t y0 = r.y < LIST_TOP ? LIST_TOP : r.y;
  int16_t y1 = (int16_t)(r.y + r.h);
  if (y1 > UI_H) y1 = UI_H;
  if (y1 <= y0) return false;
  *out = ui::Rect{ r.x, y0, r.w, (int16_t)(y1 - y0) };
  return true;
}

bool pageSettingsScrollBy(int16_t dy) {
  if (dy == 0) return false;
  s_flinging = false;
  s_bouncing = false;
  s_flingVel = 0.0f;
  s_scrollTween = false;
  const float before = s_scrollY;
  s_scrollY += (float)dy;
  clampScroll();
  if (s_scrollY != before) {
    s_confirmRow = -1;
    return true;
  }
  return false;
}

bool pageSettingsTouchDown(int16_t x, int16_t y, uint32_t nowMs) {
  s_flinging = false;
  s_bouncing = false;
  s_flingVel = 0.0f;
  s_scrollTween = false;

  const Hit h = hitTest(x, y);
  if (h.row >= 0 && h.zone == 2) {
    s_confirmRow = -1;
    s_dragRow = h.row;
    s_scrollTouchActive = false;
    applySliderX(x);
    return true;
  }

  s_scrollTouchActive = true;
  s_lastTouchY = y;
  s_sampleCount = 1;
  s_samples[0] = { y, nowMs };
  return false;
}

bool pageSettingsTouchMove(int16_t x, int16_t, uint32_t) {
  if (s_dragRow < 0) return false;
  applySliderX(x);
  return true;
}

UiAction pageSettingsTouchUp(uint32_t) {
  if (s_dragRow < 0) return UiAction{};
  const uint8_t id = kRows[s_dragRow].id;
  const uint16_t v = rowValue(settingsGet(), id);
  s_dragRow = -1;
  return UiAction{ rowActionKind(id), (int)v };
}

bool pageSettingsScrollTouchMove(int16_t y, uint32_t nowMs) {
  if (!s_scrollTouchActive) {
    s_scrollTouchActive = true;
    s_lastTouchY = y;
    s_sampleCount = 1;
    s_samples[0] = { y, nowMs };
    return false;
  }

  const int16_t dy = (int16_t)(y - s_lastTouchY);
  s_lastTouchY = y;

  if (s_sampleCount == 0 ||
      nowMs != s_samples[s_sampleCount - 1].ms ||
      y != s_samples[s_sampleCount - 1].y) {
    if (s_sampleCount < 3) {
      s_samples[s_sampleCount++] = { y, nowMs };
    } else {
      s_samples[0] = s_samples[1];
      s_samples[1] = s_samples[2];
      s_samples[2] = { y, nowMs };
    }
  }

  const float maxS = maxScrollF();
  const float prev = s_scrollY;
  float next = prev;
  float remaining = -(float)dy;

  if (remaining != 0.0f) {
    if (next < 0.0f) {
      if (remaining > 0.0f) {
        const float fingerToZero = -next / OVERSCROLL_DAMP;
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
        const float fingerToMax = (next - maxS) / OVERSCROLL_DAMP;
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
      const float cand = next + remaining;
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
  s_confirmRow = -1;
  return true;
}

void pageSettingsScrollTouchUp(uint32_t nowMs) {
  s_scrollTouchActive = false;
  const float maxS = maxScrollF();
  if (s_scrollY < 0.0f) {
    startBounce(0.0f, nowMs);
    s_sampleCount = 0;
    return;
  }
  if (s_scrollY > maxS) {
    startBounce(maxS, nowMs);
    s_sampleCount = 0;
    return;
  }

  if (s_sampleCount >= 2) {
    const TouchSample& first = s_samples[0];
    const TouchSample& last  = s_samples[s_sampleCount - 1];
    const int32_t staleMs = (int32_t)(nowMs - last.ms);
    const int32_t dtMs    = (int32_t)(last.ms - first.ms);
    if (staleMs <= 100 && dtMs > 0 && dtMs <= 200) {
      float v = ((float)(first.y - last.y) / (float)dtMs) * 16.67f;
      if (v > 80.0f) v = 80.0f;
      if (v < -80.0f) v = -80.0f;
      if (fabsf(v) >= FLING_STOP_V) {
        s_flinging = true;
        s_flingVel = v;
        s_lastTickMs = nowMs;
      }
    }
  }
  s_sampleCount = 0;
}

void pageSettingsScrollCancel() {
  s_scrollTouchActive = false;
  s_sampleCount = 0;
}
