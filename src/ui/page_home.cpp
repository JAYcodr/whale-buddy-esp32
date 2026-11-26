#include "qrcode.h"
#include "ui/pages.h"
#include "anim/nva.h"
#include "ui/theme.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr ui::Rect BTN_CANCEL_PORTAL{ 94, 384, 180, 44 };  // AP 页重排后按钮下移，不压二维码
constexpr int16_t STAGE_TOP    = 62;
constexpr int16_t STAGE_BOTTOM = 304;
constexpr int16_t PET_MIN_X    = 80;
constexpr int16_t PET_MAX_X    = 288;
constexpr int16_t PET_BASE_Y   = 298;
constexpr ui::Rect PILL_RECT{ 86, 308, 196, 28 };

// ── AP 配网二维码 ────────────────────────────────────────────────────────
// 载荷是 Wi-Fi 标准格式，扫完只负责连上设备热点；家里 WiFi 与 Broker 仍走
// Captive Portal（192.168.4.1）。
//
// 用核心自带的 esp_qrcode（espressif__qrcode 0.1.0~2，随 framework 分发、
// 默认已链接，不需要 lib_deps）。三个必须注意的点：
//   1. 回调是同步的，返回前就把内部缓冲 free 了 —— 不能保存 handle，
//      也只能在这里把点阵抄出来。0.1.0~2 的 config 没有 user_data 字段，
//      Canvas 指针只能走静态变量。
//   2. 实际编码版本由载荷长度决定（27 字节 → v2 / 25 模块）；
//      max_qrcode_version 只影响 calloc 的缓冲大小，设太大只是白占堆。
//   3. 静区（quiet zone）要自己留 4 模块。组件的 print_console 只用 2，
//      照抄会扫不出。
namespace {
constexpr int kQrQuiet = 4;        // ISO 18004 要求至少 4 模块
constexpr int kQrMaxModules = 64;

struct QrBitmap {
  int  size = 0;                   // 模块数（不含静区）
  bool ready = false;
  bool mod[kQrMaxModules][kQrMaxModules] = {};
};

QrBitmap s_qr;
char     s_qrPayload[64] = "";     // 记住上次编码的载荷，变了才重建

void qrCapture(esp_qrcode_handle_t h) {
  const int n = esp_qrcode_get_size(h);
  if (n <= 0 || n > kQrMaxModules) { s_qr.ready = false; return; }
  s_qr.size = n;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      s_qr.mod[y][x] = esp_qrcode_get_module(h, x, y);
    }
  }
  s_qr.ready = true;
}

// 懒生成：只在 AP 态第一次绘制时算一次，之后只 blit。
// 不在 onApStart 里做 —— 那个回调跑在网络任务，跨任务写共享位图有撕裂风险。
void qrEnsure(const char* payload) {
  if (s_qr.ready && strcmp(s_qrPayload, payload) == 0) return;
  snprintf(s_qrPayload, sizeof(s_qrPayload), "%s", payload);
  s_qr.ready = false;

  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = qrCapture;
  cfg.max_qrcode_version = 4;      // 27 字节只需 v2，留到 v4 够用且省堆
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  if (esp_qrcode_generate(&cfg, payload) != ESP_OK) {
    s_qr.ready = false;
  }
}

// 画到白底卡片上。不做反色 —— 反色码在部分扫码器上读不出。
void qrDraw(NovaCanvas& c, int16_t cx, int16_t cy, int16_t side) {
  const uint16_t white = rgb565(255, 255, 255);
  const uint16_t black = rgb565(0, 0, 0);
  c.fillRect(cx, cy, side, side, white);
  if (!s_qr.ready) return;

  const int total = s_qr.size + kQrQuiet * 2;
  const int scale = side / total;
  if (scale < 1) return;

  // 整数缩放，非整数会让模块边缘糊掉。
  const int16_t px = (int16_t)(cx + (side - total * scale) / 2);
  const int16_t py = (int16_t)(cy + (side - total * scale) / 2);
  for (int y = 0; y < s_qr.size; y++) {
    for (int x = 0; x < s_qr.size; x++) {
      if (!s_qr.mod[y][x]) continue;
      c.fillRect((int16_t)(px + (x + kQrQuiet) * scale),
                 (int16_t)(py + (y + kQrQuiet) * scale),
                 (int16_t)scale, (int16_t)scale, black);
    }
  }
}
}  // namespace

// 主屏码头矩形：高度跟有没有会话名/工具名走，绘制与命中共用。
ui::Rect dockRect(const NovaState& st) {
  const bool hasDetail = (st.sessTitle[0] || st.lastTool[0]);
  const int16_t h = hasDetail ? 88 : 80;
  return ui::Rect{ (int16_t)(20 + ui::burnOffsetX(st.burnInStep)),
                   (int16_t)(428 - h + ui::burnOffsetY(st.burnInStep)),
                   328, h };
}

// AP 页按钮要跟防烧屏偏移一起动；绘制与命中判定共用这一个矩形，
// 两边各算一次迟早会差一格。
ui::Rect apCancelPortalRect(const NovaState& st) {
  return ui::Rect{ (int16_t)(BTN_CANCEL_PORTAL.x + ui::burnOffsetX(st.burnInStep)),
                   (int16_t)(BTN_CANCEL_PORTAL.y + ui::burnOffsetY(st.burnInStep)),
                   BTN_CANCEL_PORTAL.w, BTN_CANCEL_PORTAL.h };
}

ui::Rect statusPillRect(const NovaState& st) {
  return ui::Rect{ (int16_t)(PILL_RECT.x + ui::burnOffsetX(st.burnInStep)),
                   (int16_t)(PILL_RECT.y + ui::burnOffsetY(st.burnInStep)),
                   PILL_RECT.w, PILL_RECT.h };
}

bool containsCaseInsensitive(const char* haystack, const char* needle) {
  if (!haystack || !needle || !needle[0]) return false;
  for (; *haystack; haystack++) {
    const char* a = haystack;
    const char* b = needle;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      a++;
      b++;
    }
    if (*b == '\0') return true;
  }
  return false;
}

float    s_petX             = 184.0f;
float    s_targetX          = 184.0f;
bool     s_runSpeed         = false;
bool     s_userTargetLock   = false;
bool     s_facingRight      = false;
uint32_t s_lastStepFrame    = 0;
uint32_t s_prevRoamMs       = 0;
uint32_t s_walkLeadUntilMs  = 0;
uint32_t s_nextRoamMs       = 8000;
int      s_sparkFrames      = 0;
int      s_demoAnim         = -1;  // -1 = auto
bool     s_idleResting      = false;

uint32_t s_seenChoreoGen    = 0;
uint8_t  s_choreoIdx        = 0;
bool     s_choreoArrived    = true;
bool     s_choreoStepInit   = false;
uint32_t s_choreoStepEnd    = 0;
uint32_t s_choreoStartCycle = 0;
int      s_choreoAnim       = -1;

uint32_t s_squashStartMs    = 0;
bool     s_squashActive     = false;
bool     s_pendingSquashTap = false;

char     s_bubbleBuf[64]    = "";
uint32_t s_bubbleUntilMs    = 0;
uint32_t s_pendingBubbleMs  = 0;

uint32_t   s_comboTimes[2]  = {};
uint8_t    s_comboCount     = 0;
uint8_t    s_clickPoolIdx   = 0;
NvaAnimKey s_lastClickKey   = NvaAnimKey::COUNT;

int16_t s_prevX0 = 72, s_prevY0 = 62, s_prevX1 = 296, s_prevY1 = 304;
bool    s_hasPrevBox = false;

const char* const kClickWaveQuotes[] = {
  "在呢主人!",
  "随时待命哦~",
};
const char* const kClickHappyQuotes[] = {
  "摸摸头充满电!",
  "今天也要加油!",
};
const char* const kClickShyQuotes[] = {
  "突然戳我干嘛呀~",
  "咦，别一直盯着看!",
};
const char* const kClickGiggleQuotes[] = {
  "嘿嘿，好痒呀~",
  "哈哈，别挠啦!",
};
const char* const kClickAngryQuotes[] = {
  "别老戳人家啦!",
  "再戳要生气咯!",
};
const char* const kWakeQuotes[] = {
  "呜哇! 没在偷懒!",
  "唔...醒啦主人!",
};
uint8_t s_quoteIdx = 0;

void copyUtf8Safe(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!src || !src[0]) return;
  size_t i = 0;
  while (src[i]) {
    size_t nx = ui::nextUtf8Boundary(src, i);
    if (nx >= cap) break;
    i = nx;
  }
  memcpy(dst, src, i);
  dst[i] = '\0';
}

void setBubble(const char* txt, uint32_t durMs, uint32_t nowMs = 0) {
  if (!txt || !txt[0]) return;
  copyUtf8Safe(s_bubbleBuf, sizeof(s_bubbleBuf), txt);
  if (nowMs > 0) {
    s_bubbleUntilMs = nowMs + durMs;
    s_pendingBubbleMs = 0;
  } else {
    s_pendingBubbleMs = durMs;
  }
}

// Ported from dsh-pet/src/shared/physics/squash.ts: ease-in compress (0..0.45) + easeOutBack bounce (0.45..1).
float calcSquashScaleY(uint32_t nowMs) {
  if (!s_squashActive) return 1.0f;
  uint32_t elapsed = nowMs - s_squashStartMs;
  constexpr uint32_t SQ_DUR_MS = 320;
  constexpr float SQ_MIN = 0.86f;
  if (elapsed >= SQ_DUR_MS) {
    s_squashActive = false;
    return 1.0f;
  }
  float u = (float)elapsed / (float)SQ_DUR_MS;
  if (u < 0.45f) {
    float p = u / 0.45f;
    return 1.0f - (1.0f - SQ_MIN) * p * p;
  }
  float p = (u - 0.45f) / 0.55f;
  float pm1 = p - 1.0f;
  constexpr float c1 = 1.70158f;
  constexpr float c3 = c1 + 1.0f;
  float f = 1.0f + c3 * pm1 * pm1 * pm1 + c1 * pm1 * pm1;
  if (f < 0.0f) f = 0.0f;
  float sy = SQ_MIN + (1.0f - SQ_MIN) * f;
  return (sy > 1.10f) ? 1.10f : sy;
}

bool spriteAllowsMirror(NvaAnimKey k) {
  switch (k) {
    case NvaAnimKey::IDLE:
    case NvaAnimKey::SUCCESS:
    case NvaAnimKey::CLICK:
    case NvaAnimKey::SLEEP:
    case NvaAnimKey::CLICK_HAPPY:
    case NvaAnimKey::CLICK_SHY:
    case NvaAnimKey::CLICK_GIGGLE:
    case NvaAnimKey::CRAB_WALK:
    case NvaAnimKey::HAPPY_RUN:
    case NvaAnimKey::STRETCH:
    case NvaAnimKey::YAWN:
    case NvaAnimKey::WAKE_UP:
    case NvaAnimKey::HUM:
    case NvaAnimKey::BUBBLE:
      return true;
    default:
      return false;
  }
}

void drawSpark(NovaCanvas& c, int16_t x, int16_t y, uint16_t col, int16_t size) {
  ui::aaFillCircle(c, x, y, (size >= 3) ? 2 : 1, col);
  c.drawLine(x - size - 1, y, x + size + 1, y, col);
  c.drawLine(x, y - size - 1, x, y + size + 1, col);
}

void updateRoam(const NovaState& st, const NovaSettings& cfg, uint32_t frame) {
  if (frame == s_lastStepFrame) return;
  s_lastStepFrame = frame;

  if (s_pendingSquashTap) {
    s_pendingSquashTap = false;
    pageHomeTriggerSquash(st.nowMs);
  }
  if (s_pendingBubbleMs > 0) {
    s_bubbleUntilMs = st.nowMs + s_pendingBubbleMs;
    s_pendingBubbleMs = 0;
  }

  uint32_t dtMs = (s_prevRoamMs == 0 || st.nowMs <= s_prevRoamMs) ? 33u : (st.nowMs - s_prevRoamMs);
  if (dtMs > 120u) dtMs = 120u;
  s_prevRoamMs = st.nowMs;

  // 睡眠/哈欠停步闸门只看端侧真实闲置（s_idleResting）或手动 Demo 锁，
  // 绝不能反读 nvaActiveState() == SLEEP/YAWN，否则导演剧本演 sleep/yawn 时会触发自毁重进死循环。
  const bool resting = s_idleResting ||
                       s_demoAnim == (int)NvaAnimKey::SLEEP ||
                       s_demoAnim == (int)NvaAnimKey::YAWN;
  if (resting && !s_userTargetLock) {
    s_targetX = s_petX;
    s_runSpeed = false;
  }

  float diff = s_targetX - s_petX;
  float adiff = diff < 0 ? -diff : diff;
  if (adiff > 6.0f) s_facingRight = (diff > 0.0f);

  if (adiff > 1.2f) {
    if (s_walkLeadUntilMs == 0 || (int32_t)(st.nowMs - s_walkLeadUntilMs) >= 0) {
      float maxSpeedPxPerSec = s_runSpeed ? 92.0f : 38.0f;
      if (adiff < 16.0f) {
        float factor = adiff / 16.0f;
        if (factor < 0.35f) factor = 0.35f;
        maxSpeedPxPerSec *= factor;
      }
      float step = maxSpeedPxPerSec * ((float)dtMs / 1000.0f);
      if (adiff <= step) {
        s_petX = s_targetX;
        s_runSpeed = false;
        s_userTargetLock = false;
      } else {
        s_petX += (diff > 0) ? step : -step;
      }
    }
  } else {
    s_petX = s_targetX;
    s_runSpeed = false;
    s_userTargetLock = false;
  }

  if (st.choreoGen != s_seenChoreoGen) {
    s_seenChoreoGen = st.choreoGen;
    s_choreoIdx = 0;
    s_choreoArrived = false;
    s_choreoStepInit = false;
    s_choreoStepEnd = 0;
  }

  // Execute LLM choreography script when enabled and in autonomous idle/patrol mode.
  // 犯困/熟睡期间（75s..AOD，或关闭 AOD 后的长静置）必须暂停剧本推步，
  // 否则下一步 step.x / step.say 会把熟睡中的诺瓦拉起来“梦游螃蟹步”并顶着 Zzz 弹元气台词。
  if (cfg.choreoEnabled && st.nChoreoSteps > 0 && s_demoAnim < 0 && st.run == 0 && st.wait == 0 &&
      !st.approvalActive && !s_userTargetLock && !resting) {
    if (!s_choreoStepInit) {
      if (s_choreoIdx >= st.nChoreoSteps) s_choreoIdx = 0;
      const ChoreoStep& step = st.choreoSteps[s_choreoIdx];
      const bool sleepyStep = (step.anim == (uint8_t)NvaAnimKey::SLEEP ||
                               step.anim == (uint8_t)NvaAnimKey::YAWN);
      s_targetX = sleepyStep ? s_petX : (float)step.x;
      float walkDist = fabsf(s_targetX - s_petX);
      s_runSpeed = sleepyStep ? false : step.run;
      s_choreoAnim = step.anim;
      s_walkLeadUntilMs = (walkDist > 6.0f) ? (st.nowMs + 220u) : 0;
      s_choreoArrived = (walkDist <= 1.2f);
      uint32_t dur = step.durMs >= 10000u ? step.durMs : 10000u;
      s_choreoStepEnd = st.nowMs + dur;
      s_choreoStartCycle = nvaLoopCycles();
      s_choreoStepInit = true;
      if (step.say[0]) setBubble(step.say, dur > 8500u ? 8500u : dur, st.nowMs);
      return;
    }

    float remWalk = fabsf(s_targetX - s_petX);
    if (!s_choreoArrived && remWalk <= 1.2f) {
      s_choreoArrived = true;
      const ChoreoStep& cur = st.choreoSteps[s_choreoIdx];
      uint32_t dur = cur.durMs >= 10000u ? cur.durMs : 10000u;
      s_choreoStepEnd = st.nowMs + dur;
      s_choreoStartCycle = nvaLoopCycles();
    }

    if (s_choreoArrived && (int32_t)(st.nowMs - s_choreoStepEnd) >= 0) {
      bool cycleComplete = (nvaLoopCycles() > s_choreoStartCycle);
      bool safetyExpired = ((int32_t)(st.nowMs - (s_choreoStepEnd + 5000u)) >= 0);
      if (cycleComplete || safetyExpired) {
        s_choreoIdx = (uint8_t)((s_choreoIdx + 1) % st.nChoreoSteps);
        s_choreoStepInit = false;
      }
    }
    return;
  }
  s_choreoAnim = -1;
  s_choreoStepInit = false;

  // Built-in local fallback patrol when no LLM script is active.
  if (s_petX == s_targetX && (int32_t)(st.nowMs - s_nextRoamMs) >= 0 && st.wait == 0 && !st.approvalActive) {
    if (resting) {
      s_nextRoamMs = st.nowMs + 12000u;
      return;
    }
    static constexpr int16_t kStops[6] = { 108, 184, 260, 140, 228, 184 };
    uint32_t idx = (st.nowMs / 13000u + st.second) % 6;
    s_targetX = (float)kStops[idx];
    s_walkLeadUntilMs = st.nowMs + 220u;
    s_runSpeed = (st.run > 0);
    s_nextRoamMs = st.nowMs + (st.run > 0 ? 13000u : 18000u);
  }
}

void drawStageContent(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame,
                      int16_t* outDx, int16_t* outDy, int16_t* outDw, int16_t* outDh) {
  updateRoam(st, cfg, frame);

  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const int16_t cx = (int16_t)(s_petX + 0.5f) + ox;

  float diff = s_targetX - s_petX;
  bool walking = (diff < -1.2f || diff > 1.2f);
  int16_t hop = walking ? (int16_t)(fabsf(sinf(frame * 0.28f)) * 2.0f) : 0;
  const int16_t bottomY = PET_BASE_Y + oy - hop;

  const int16_t sprW = nvaFrameWidth();
  const int16_t sprH = nvaFrameHeight();
  int16_t halfW = (sprW / 2) + 12;
  if (halfW < 88) halfW = 88;
  int16_t curX0 = cx - halfW;
  int16_t curX1 = cx + halfW;
  int16_t curY0 = bottomY - sprH - 12;
  int16_t curY1 = PET_BASE_Y + oy + 6;

  bool showBubble = (s_bubbleBuf[0] && s_bubbleUntilMs != 0 && (int32_t)(s_bubbleUntilMs - st.nowMs) > 0);
  if (s_bubbleBuf[0] && !showBubble && s_bubbleUntilMs != 0) {
    s_bubbleBuf[0] = '\0';
    s_bubbleUntilMs = 0;
  }

  int16_t bx = 12, by = STAGE_TOP + 6, bw = 120;
  if (showBubble) {
    ui::TextMetrics bm = ui::measureText(c, ui::fontCjk14(), s_bubbleBuf);
    bw = (int16_t)bm.w + 26;
    if (bw > UI_W - 24) bw = UI_W - 24;
    bx = cx - bw / 2;
    if (bx < 12) bx = 12;
    if (bx + bw > UI_W - 12) bx = UI_W - 12 - bw;

    by = bottomY - sprH - 36;
    if (by < STAGE_TOP + 4) by = STAGE_TOP + 4;
    if (by > STAGE_BOTTOM - 34) by = STAGE_BOTTOM - 34;

    if (bx - 2 < curX0) curX0 = bx - 2;
    if (bx + bw + 2 > curX1) curX1 = bx + bw + 2;
    if (by - 2 < curY0) curY0 = by - 2;
  }

  if (curX0 < 0) curX0 = 0;
  if (curX1 > UI_W) curX1 = UI_W;
  if (curY0 < STAGE_TOP) curY0 = STAGE_TOP;
  if (curY1 > STAGE_BOTTOM) curY1 = STAGE_BOTTOM;

  int16_t uX0 = s_hasPrevBox ? (curX0 < s_prevX0 ? curX0 : s_prevX0) : 0;
  int16_t uY0 = s_hasPrevBox ? (curY0 < s_prevY0 ? curY0 : s_prevY0) : STAGE_TOP;
  int16_t uX1 = s_hasPrevBox ? (curX1 > s_prevX1 ? curX1 : s_prevX1) : UI_W;
  int16_t uY1 = s_hasPrevBox ? (curY1 > s_prevY1 ? curY1 : s_prevY1) : STAGE_BOTTOM;

  s_prevX0 = curX0; s_prevY0 = curY0; s_prevX1 = curX1; s_prevY1 = curY1;
  s_hasPrevBox = true;

  c.fillRect(uX0, uY0, uX1 - uX0, uY1 - uY0, ui::COL_BG);

  const float t = frame * 0.06f;
  const bool busy = (st.run > 0 || st.wait > 0);
  const uint16_t auraCol = st.wait > 0 ? ui::COL_AMBER : (busy ? ui::COL_MINT : ui::COL_ACCENT);

  for (int i = 0; i < 3; i++) {
    float a = t * (busy ? 1.25f : 0.65f) + i * 2.0944f;
    int16_t px = cx + (int16_t)(cosf(a) * 76.0f);
    int16_t py = (bottomY - 88) + (int16_t)(sinf(a * 1.15f) * 38.0f);
    if (px >= curX0 + 3 && px <= curX1 - 3 && py >= curY0 + 3 && py <= curY1 - 3) {
      ui::aaFillCircle(c, px, py, (i & 1) ? 2 : 3, (i & 1) ? ui::COL_MINT : ui::COL_ACCENT);
    }
  }

  if (s_sparkFrames > 0) {
    float prog = 1.0f - (float)s_sparkFrames / 24.0f;
    for (int i = 0; i < 6; i++) {
      float a = prog * 0.7f + i * 1.0472f;
      int d = 44 + (int)(prog * 38.0f);
      int16_t sx = cx + (int16_t)(cosf(a) * d);
      int16_t sy = (bottomY - 92) + (int16_t)(sinf(a) * d) - (int16_t)(prog * 12.0f);
      if (sx >= curX0 + 4 && sx <= curX1 - 4 && sy >= curY0 + 4 && sy <= curY1 - 4) {
        drawSpark(c, sx, sy, (i & 1) ? ui::COL_MINT : ui::COL_GOLD, (s_sparkFrames > 12) ? 3 : 2);
      }
    }
    s_sparkFrames--;
  }

  const NvaAnimKey activeState = nvaActiveState();
  const bool flipX = s_facingRight && spriteAllowsMirror(activeState);
  float squashY = calcSquashScaleY(st.nowMs);
  if (!nvaDraw(c, cx, bottomY, flipX, squashY)) {
    ui::aaFillCircle(c, cx, bottomY - 76, 36, auraCol);
    ui::aaFillCircle(c, cx - 10, bottomY - 86, 9, rgb565(224, 246, 255));
  }

  // 熟睡阶段在头顶斜上方画缓慢上浮淡出的 Zzz 微粒子，严格包在脏矩形 curX0..curY1 内。
  if (activeState == NvaAnimKey::SLEEP) {
    const float dir = flipX ? -1.0f : 1.0f;
    for (int i = 0; i < 3; i++) {
      float u = fmodf((float)frame * 0.022f + (float)i * 0.333f, 1.0f);
      float env = (u < 0.25f) ? (u / 0.25f) : (1.0f - (u - 0.25f) / 0.75f);
      if (env < 0.08f) continue;
      uint8_t alpha = (uint8_t)(env * 210.0f);
      uint16_t zCol = ui::blend565(ui::COL_BG, (i == 1) ? ui::COL_ACCENT : ui::COL_VIOLET, alpha);
      int16_t zx = (int16_t)(cx + dir * (28.0f + u * 24.0f + sinf(u * 6.283f) * 3.0f))
                   - (flipX ? 10 : 0);
      int16_t zy = (int16_t)(bottomY - 116 - u * 34.0f);
      if (zx < curX0 + 2 || zx + 14 > curX1 - 2) continue;
      if (zy - 16 < curY0 + 2 || zy + 2 > curY1 - 2) continue;
      if (showBubble && zy - 16 < by + 30 && zx + 14 > bx - 2 && zx < bx + bw + 2) continue;
      ui::drawText(c, ui::fontAscii14(), zx, zy, (u > 0.55f) ? "Z" : "z", zCol, ui::COL_BG);
    }
  }

  if (showBubble) {
    ui::aaFillRoundRect(c, bx, by, bw, 28, 14, ui::COL_CARD_HI, ui::COL_ACCENT);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), { bx, by, bw, 28 }, s_bubbleBuf, ui::COL_TEXT, ui::COL_CARD_HI);
  }

  if (outDx) *outDx = uX0;
  if (outDy) *outDy = uY0;
  if (outDw) *outDw = uX1 - uX0;
  if (outDh) *outDh = uY1 - uY0;
}

void drawStatusPill(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);

  uint16_t dotCol = ui::COL_ACCENT;
  char tagBuf[48] = "待机陪伴中";
  if (s_demoAnim >= 0) {
    static constexpr const char* const kDemoTags[] = {
      "展示: 待机", "展示: 思考", "展示: 忙碌",
      "展示: 等待", "展示: 庆祝", "展示: 委屈",
      "展示: 挥手", "展示: 熟睡", "展示: 富足",
      "展示: 跃动", "展示: 害羞", "展示: 傲娇", "展示: 挠痒",
      "展示: 蟹步", "展示: 小跑", "展示: 敲码", "展示: 归档",
      "展示: 懒腰", "展示: 哈欠", "展示: 惊醒",
      "展示: 哼歌", "展示: 干饭", "展示: 冒泡", "展示: 算账",
    };
    static_assert(sizeof(kDemoTags) / sizeof(kDemoTags[0]) == (size_t)NvaAnimKey::COUNT,
                  "kDemoTags 必须与 NvaAnimKey::COUNT 同步");
    dotCol = ui::COL_VIOLET;
    copyUtf8Safe(tagBuf, sizeof(tagBuf), kDemoTags[s_demoAnim % (int)NvaAnimKey::COUNT]);
  } else if (st.wait > 0) {
    dotCol = ui::COL_AMBER;
    copyUtf8Safe(tagBuf, sizeof(tagBuf), "等待确认");
  } else if (st.run > 0) {
    dotCol = ui::COL_MINT;
    const NvaAnimKey work = pageHomeWorkAnimForTool(st.lastTool);
    if (st.activeModel[0]) {
      const char* verb = (work == NvaAnimKey::CODING) ? "敲码"
                       : (work == NvaAnimKey::ARCHIVE ? "归档" : "执行");
      char raw[48];
      snprintf(raw, sizeof(raw), "%s · %s", verb, st.activeModel);
      ui::fitUtf8Line(c, ui::fontCjk14(), raw, 224, tagBuf, sizeof(tagBuf));
    } else {
      const char* label = (work == NvaAnimKey::CODING) ? "努力敲码中"
                        : (work == NvaAnimKey::ARCHIVE ? "清点归档中"
                        : (work == NvaAnimKey::WORKING ? "忙碌处理中" : "思考执行中"));
      copyUtf8Safe(tagBuf, sizeof(tagBuf), label);
    }
  } else if (st.turnError) {
    dotCol = ui::COL_RED;
    copyUtf8Safe(tagBuf, sizeof(tagBuf), "上轮执行异常");
  } else if (nvaActiveState() == NvaAnimKey::SLEEP) {
    dotCol = ui::COL_VIOLET;
    copyUtf8Safe(tagBuf, sizeof(tagBuf), "打盹小憩中");
  } else if (nvaActiveState() == NvaAnimKey::YAWN) {
    dotCol = ui::COL_VIOLET;
    copyUtf8Safe(tagBuf, sizeof(tagBuf), "犯困打哈欠...");
  } else if (cfg.choreoEnabled && st.nChoreoSteps > 0 && st.choreoMood[0]) {
    dotCol = ui::COL_ACCENT;
    copyUtf8Safe(tagBuf, sizeof(tagBuf), st.choreoMood);
  } else if (!st.wifiConnected || !st.mqttConnected) {
    dotCol = ui::COL_ACCENT;
    copyUtf8Safe(tagBuf, sizeof(tagBuf), "本地陪伴中");
  }

  ui::TextMetrics tm = ui::measureText(c, ui::fontCjk14(), tagBuf);
  int16_t pw = (int16_t)tm.w + 34;
  if (pw > 280) pw = 280;
  int16_t px = (int16_t)((UI_W - pw) / 2 + ox);
  int16_t py = (int16_t)(308 + oy);
  ui::drawCard(c, px, py, pw, 28, ui::COL_CARD, ui::COL_BORDER, 14);
  ui::aaFillCircle(c, px + 12, py + 14, 3, dotCol);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), { (int16_t)(px + 18), py, (int16_t)(pw - 22), 28 },
                             tagBuf, ui::COL_DIM, ui::COL_CARD);
}

}  // namespace

int pageHomeDemoOverride() {
  return s_demoAnim;
}

void pageHomeSetDemoOverride(int idx) {
  s_demoAnim = (idx >= 0 && idx < (int)NvaAnimKey::COUNT) ? idx : -1;
}

int pageHomeChoreoAnim() {
  return s_choreoAnim;
}

bool pageHomeIsWalking() {
  float diff = s_targetX - s_petX;
  return (diff < -1.2f || diff > 1.2f);
}

bool pageHomeIsRunning() {
  return s_runSpeed && pageHomeIsWalking();
}

NvaAnimKey pageHomeWorkAnimForTool(const char* tool) {
  if (!tool || !tool[0]) return NvaAnimKey::THINKING;
  static constexpr const char* const kCodingTools[] = {
    "edit", "write", "patch", "bash", "terminal", "cmd", "create",
  };
  for (const char* kw : kCodingTools) {
    if (containsCaseInsensitive(tool, kw)) return NvaAnimKey::CODING;
  }
  static constexpr const char* const kArchiveTools[] = {
    "read", "grep", "glob", "search", "find", "list", "fetch",
  };
  for (const char* kw : kArchiveTools) {
    if (containsCaseInsensitive(tool, kw)) return NvaAnimKey::ARCHIVE;
  }
  return NvaAnimKey::WORKING;
}

void pageHomeTriggerSquash(uint32_t nowMs) {
  s_squashStartMs = nowMs;
  s_squashActive = true;
}

void pageHomeTick(const NovaState& st, const NovaSettings& cfg, uint32_t frame, bool idleResting) {
  s_idleResting = idleResting;
  updateRoam(st, cfg, frame);
}

void pageHomeDrawStage(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame,
                       int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH) {
  drawStageContent(c, st, cfg, frame, outX, outY, outW, outH);
}

// 异常详情浮层。舞台区 62..304 正好能放 5 行 14px 正文，
// 用 drawWrappedUtf8 自动折行，不自己算 UTF-8 边界。
void drawDockDetail(NovaCanvas& c, const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const int16_t x = (int16_t)(20 + ox);
  const int16_t y = (int16_t)(62 + oy);
  const int16_t w = 328;
  const int16_t h = 232;

  ui::drawCard(c, x, y, w, h, rgb565(8, 12, 20), ui::COL_RED, 18);
  ui::drawText(c, ui::fontCjk16(), (int16_t)(x + 18), (int16_t)(y + 32),
               "本轮异常", ui::COL_RED, rgb565(8, 12, 20));

  if (st.sessTitle[0]) {
    char titleBuf[56];
    ui::fitUtf8Line(c, ui::fontCjk14(), st.sessTitle, w - 110, titleBuf, sizeof(titleBuf));
    ui::drawTextRight(c, ui::fontCjk14(), (int16_t)(x + w - 18), (int16_t)(y + 31),
                      titleBuf, ui::COL_DIM, rgb565(8, 12, 20));
  }

  c.drawFastHLine((int16_t)(x + 18), (int16_t)(y + 46), (int16_t)(w - 36), ui::COL_BORDER);

  ui::drawWrappedUtf8(c, ui::fontCjk14(), (int16_t)(x + 18), (int16_t)(y + 76),
                      (int16_t)(w - 36), 24, 5, st.errMsg, ui::COL_TEXT, rgb565(8, 12, 20));

  ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(x + w / 2), (int16_t)(y + h - 16),
                       "轻触任意处收起", ui::COL_MUTED, rgb565(8, 12, 20));
}

void pageHomeDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame) {
  c.fillScreen(ui::COL_BG);
  ui::drawTopBar(c, st, 1);

  if (st.wifiApMode && !st.wifiConnected) {
    // AP 卡片以前写死 24,80，不跟防烧屏偏移 —— 这块一屏静止几百秒，
    // 补上二维码后就是 AMOLED 最容易烧出残影的图形。这里整块跟其他页一致地偏移；
    // 命中判定（pageHomeTap / pageHomeHitRect）必须用同一个偏移，否则按钮会点歪。
    const int16_t aox = ui::burnOffsetX(st.burnInStep);
    const int16_t aoy = ui::burnOffsetY(st.burnInStep);

    // AP 页重排：一张 297px 的码塞不进原来 320×290 的三步说明卡片里，
    // 所以改成「扫码头 + 一行提示 + 按钮」。
    ui::drawCard(c, (int16_t)(24 + aox), (int16_t)(62 + aoy), 320, 378,
                 ui::COL_CARD, ui::COL_AMBER, 20);
    ui::drawText(c, ui::fontCjk16(), (int16_t)(44 + aox), (int16_t)(92 + aoy),
                 "扫码连热点", ui::COL_AMBER, ui::COL_CARD);

    // 载荷必须是 nopass 的 Wi-Fi 标准格式；扫码只负责连上本机热点。
    const char* ssid = st.apSsid[0] ? st.apSsid : "Nova-0000";
    char payload[64];
    snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
    qrEnsure(payload);

    const int16_t qrSide = 216;
    const int16_t qrX = (int16_t)(24 + aox + (320 - qrSide) / 2);
    const int16_t qrY = (int16_t)(96 + aoy);
    qrDraw(c, qrX, qrY, qrSide);

    char ssidLine[64];
    snprintf(ssidLine, sizeof(ssidLine), "热点 %s · 无密码", ssid);
    ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(24 + aox + 160),
                         (int16_t)(qrY + qrSide + 26), ssidLine, ui::COL_TEXT, ui::COL_CARD);

    ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(24 + aox + 160),
                         (int16_t)(qrY + qrSide + 48),
                         "连上后访问 192.168.4.1 填 WiFi", ui::COL_DIM, ui::COL_CARD);

    ui::drawButton(c, apCancelPortalRect(st), "暂不配网", false, ui::COL_AMBER);
    return;
  }

  s_hasPrevBox = false;
  drawStageContent(c, st, cfg, frame, nullptr, nullptr, nullptr, nullptr);
  drawStatusPill(c, st, cfg);

  // 异常详情浮层：点码头展开，铺在舞台区上。码头本身只有两行，
  // 完整原因（最多 errMsg 136 字节）靠这块看全，不挤占日常布局。
  if (st.dockExpanded && st.turnError && st.errMsg[0]) {
    drawDockDetail(c, st);
  }

  // 几何走 dockRect()：码头不能再往上涨（胶囊底边 336，顶边须 ≥340），
  // 所以有扩展信息时只长 8px，靠第二行前缀放会话名，不新开一行。
  const ui::Rect dock = dockRect(st);
  const bool hasDetail = (st.sessTitle[0] || st.lastTool[0]);
  const int16_t dockX = dock.x;
  const int16_t dockY = dock.y;
  const int16_t dockW = dock.w;
  const int16_t dockH = dock.h;
  uint16_t dockBd = st.turnError ? ui::COL_RED : (st.wait > 0 ? ui::COL_AMBER : ui::COL_BORDER);
  ui::drawCard(c, dockX, dockY, dockW, dockH, ui::COL_CARD, dockBd, 18);

  const bool hostLinked = (st.wifiConnected && st.mqttConnected && st.lastMqttRxMs > 0);
  if (hostLinked) {
    char statsLine[64];
    snprintf(statsLine, sizeof(statsLine), "运行 %u · 等待 %u · 会话 %u", st.run, st.wait, st.sess);
    const int16_t row1Y = (int16_t)(dockY + 30);
    const int16_t row2Y = (int16_t)(dockY + (hasDetail ? 60 : 66));
    ui::drawText(c, ui::fontCjk14(), dockX + 16, row1Y, statsLine, ui::COL_TEXT, ui::COL_CARD);

    if (st.ctxPct >= 0) {
      char ctxBuf[16];
      snprintf(ctxBuf, sizeof(ctxBuf), "CTX %d%%", st.ctxPct);
      uint16_t ctxCol = st.ctxPct >= 80 ? ui::COL_RED : (st.ctxPct >= 60 ? ui::COL_AMBER : ui::COL_MINT);
      ui::TextMetrics cm = ui::measureText(c, ui::fontAscii14(), ctxBuf);
      int16_t pw = (int16_t)cm.w + 18;
      int16_t px = (int16_t)(dockX + dockW - 14 - pw);
      int16_t py = (int16_t)(dockY + 12);
      ui::aaFillRoundRect(c, px, py, pw, 24, 12, rgb565(18, 30, 48), rgb565(18, 30, 48));
      ui::drawTextCenteredInRect(c, ui::fontAscii14(), { px, py, pw, 24 }, ctxBuf, ctxCol, rgb565(18, 30, 48));
    }

    // 第二行：异常时优先给原因原文，其余情况给当前消息。
    // 异常带「▾」角标：点码头展开全文（dockExpanded），否则这一行只能装一句话。
    char metaLine[160];
    uint16_t metaCol = ui::COL_DIM;
    int16_t metaMaxW = (int16_t)(dockX + dockW - 14 - (dockX + 16));

    // 会话名做第二行的前缀（灰色小字），而不是跟 CTX 抢第一行右端。
    // 宽度按可用空间分：给标题最多 55%，剩下的留给内容；
    // 以前写死 108px，实战里「issue 核查与复盘」被砍成「issue 核查与...」。
    int16_t metaX = (int16_t)(dockX + 16);
    if (st.sessTitle[0]) {
      int16_t avail = (int16_t)(dockX + dockW - 14 - metaX);
      if (st.turnError) avail = (int16_t)(avail - 62);  // 给「详情」角标让位
      int16_t titleMaxW = (int16_t)(avail * 55 / 100);
      if (titleMaxW < 90) titleMaxW = 90;

      char titleBuf[56];
      ui::fitUtf8Line(c, ui::fontCjk14(), st.sessTitle, titleMaxW, titleBuf, sizeof(titleBuf));
      ui::drawText(c, ui::fontCjk14(), metaX, row2Y, titleBuf, ui::COL_MUTED, ui::COL_CARD);
      ui::TextMetrics tm = ui::measureText(c, ui::fontCjk14(), titleBuf);
      metaX = (int16_t)(metaX + tm.w + 10);
      metaMaxW = (int16_t)(metaMaxW - (tm.w + 10));
    }

    // 异常时右侧有「详情」角标，正文得先把那 62px 让出来，
    // 否则正文会顶到角标底下糊成一团。
    if (st.turnError) {
      metaMaxW = (int16_t)(metaMaxW - 62);
    }

    if (st.turnError) {
      const char* errTxt = st.errMsg[0] ? st.errMsg : (st.msg[0] ? st.msg : "本轮异常结束，请检查主机");
      ui::fitUtf8Line(c, ui::fontCjk14(), errTxt, metaMaxW, metaLine, sizeof(metaLine));
      metaCol = ui::COL_RED;
    } else if ((st.run > 0 || st.wait > 0) && st.msg[0]) {
      ui::fitUtf8Line(c, ui::fontCjk14(), st.msg, metaMaxW, metaLine, sizeof(metaLine));
      metaCol = (st.wait > 0) ? ui::COL_AMBER : ui::COL_ACCENT;
    } else if (st.activeModel[0]) {
      char raw[80];
      snprintf(raw, sizeof(raw), "当前模型 · %s", st.activeModel);
      ui::fitUtf8Line(c, ui::fontCjk14(), raw, metaMaxW, metaLine, sizeof(metaLine));
    } else if (st.msg[0] && strcmp(st.msg, "待命中") != 0) {
      ui::fitUtf8Line(c, ui::fontCjk14(), st.msg, metaMaxW, metaLine, sizeof(metaLine));
    } else if (st.sessTitle[0]) {
      // 有会话名但没有活动消息时，第二行留给工具名，别重复标题。
      if (st.lastTool[0]) snprintf(metaLine, sizeof(metaLine), "工具 %s", st.lastTool);
      else metaLine[0] = '\0';
    } else {
      snprintf(metaLine, sizeof(metaLine), "轻触舞台逗诺瓦跑动 · 左右划切页");
    }
    if (metaLine[0]) {
      ui::drawText(c, ui::fontCjk14(), metaX, row2Y, metaLine, metaCol, ui::COL_CARD);
    }
    if (st.turnError) {
      ui::drawTextRight(c, ui::fontCjk14(), dockX + dockW - 14, row2Y,
                        st.dockExpanded ? "收起" : "详情", ui::COL_RED, ui::COL_CARD);
    }

  } else {
    ui::drawText(c, ui::fontCjk14(), dockX + 16, dockY + 30, "离线陪伴模式 · 本地交互就绪", ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), dockX + 16, dockY + 62, "点空白处让诺瓦跑过去 · 点胶囊换动作", ui::COL_DIM, ui::COL_CARD);
  }
}


// ── AOD（熄屏显示）─────────────────────────────────────────────────────
// 布局按 HTML 提案 A 版、字号 92 落地：时间块在 0..236 区域居中，宠物定格在
// 242..418，底部一行状态。这里不跑动画：调用方在 AOD 期间跳过 nvaTick，
// 所以 nvaDraw 画到的是最后一帧（SLEEP 的睡觉姿势）。
void formatTokenShort(uint64_t v, char* out, size_t cap) {
  if (v >= 100000000ull) {
    snprintf(out, cap, "%.2f 亿", (double)v / 1e8);
  } else if (v >= 10000ull) {
    snprintf(out, cap, "%.0f 万", (double)v / 1e4);
  } else {
    snprintf(out, cap, "%llu", (unsigned long long)v);
  }
}

// 时间行：DIN 92 只有数字，冒号必须矢量画；"--:--" 也走同一条路径，
// 否则 ':' 会落到 20px 的 fallback 字体上，出现两个小点挤在大号破折号下面。
void drawAodTimeLine(NovaCanvas& c, const char* hh, const char* mm, int16_t baseY, int16_t ox) {
  ui::TextMetrics mh = ui::measureText(c, ui::fontNum92(), hh);
  ui::TextMetrics mm2 = ui::measureText(c, ui::fontNum92(), mm);
  const int16_t colonW = 26;
  const int16_t totalW = (int16_t)(mh.w + colonW + mm2.w);
  const int16_t x0 = (int16_t)((UI_W - totalW) / 2 + ox);
  ui::drawText(c, ui::fontNum92(), x0, baseY, hh, ui::COL_TEXT, ui::COL_BG);
  const int16_t colonX = (int16_t)(x0 + mh.w + colonW / 2);
  ui::aaFillCircle(c, colonX, (int16_t)(baseY - 52), 5, ui::COL_TEXT);
  ui::aaFillCircle(c, colonX, (int16_t)(baseY - 24), 5, ui::COL_TEXT);
  ui::drawText(c, ui::fontNum92(), (int16_t)(x0 + mh.w + colonW), baseY, mm,
               ui::COL_TEXT, ui::COL_BG);
}

void pageHomeDrawAod(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  c.fillScreen(ui::COL_BG);

  // 位移：30s 一次的 ±2px（沿用 burnInStep）+ 10 分钟一轮的 ±3px 慢漂移。
  const int16_t ox = (int16_t)(ui::burnOffsetX(st.burnInStep) + aod::driftX(nowMs));
  const int16_t oy = (int16_t)(ui::burnOffsetY(st.burnInStep) + aod::driftY(nowMs));

  // 右上：电量 + 在线点（不画顶栏时钟、不画页面圆点）。
  ui::drawBatteryIcon(c, (int16_t)(UI_SAFE_R - 4 + ox), (int16_t)(29 + oy),
                      st.battPct, st.usbPresent, st.battStage);
  const bool linked = (st.wifiConnected && st.mqttConnected);
  const uint16_t dotCol = st.wifiApMode ? ui::COL_AMBER : (linked ? ui::COL_MINT : ui::COL_RED);
  ui::aaFillCircle(c, (int16_t)(UI_SAFE_R - 64 + ox), (int16_t)(29 + oy), 3, dotCol);

  if (!st.timeValid) {
    // 没有 RTC 时不要用大号假数字（DIN 92 的 '-' 会像下划线，'--:--' 很难看）。
    ui::drawTextCentered(c, ui::fontCjk16(), (int16_t)(UI_W / 2 + ox), (int16_t)(158 + oy),
                         "时间未同步", ui::COL_DIM, ui::COL_BG);
    ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(UI_W / 2 + ox), (int16_t)(194 + oy),
                         "等待宿主校时", ui::COL_MUTED, ui::COL_BG);
  } else {
    char hh[3], mm[3];
    snprintf(hh, sizeof(hh), "%02u", (unsigned)st.hour);
    snprintf(mm, sizeof(mm), "%02u", (unsigned)st.minute);
    drawAodTimeLine(c, hh, mm, (int16_t)(150 + oy), ox);

    static const char* const kDow[7] = { "日", "一", "二", "三", "四", "五", "六" };
    char dateBuf[40];
    snprintf(dateBuf, sizeof(dateBuf), "%u月%u日 · 周%s",
             (unsigned)st.month, (unsigned)st.day, kDow[st.dow % 7]);
    ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(UI_W / 2 + ox), (int16_t)(194 + oy),
                         dateBuf, ui::COL_DIM, ui::COL_BG);
  }

  // 宠物：定格睡觉帧
  if (!nvaDraw(c, (int16_t)(UI_W / 2 + ox), (int16_t)(418 + oy), false, 1.0f)) {
    ui::aaFillCircle(c, (int16_t)(UI_W / 2 + ox), (int16_t)(400 + oy), 34, rgb565(20, 32, 52));
  }

  // 底部状态：AOD 里 run 恒为 0，不写它。
  char today[24] = "";
  if (st.nTokenDays > 0) {
    formatTokenShort(st.tokenDays[st.nTokenDays - 1].tokens, today, sizeof(today));
  }
  char statusBuf[72];
  if (st.run > 0) {
    // 长任务期间也进 AOD，所以「还在干活」必须留在表盘上，不能只剩一个钟。
    if (today[0]) snprintf(statusBuf, sizeof(statusBuf), "运行 %u · 会话 %u · 今日 %s",
                           (unsigned)st.run, (unsigned)st.sess, today);
    else snprintf(statusBuf, sizeof(statusBuf), "运行 %u · 会话 %u",
                  (unsigned)st.run, (unsigned)st.sess);
  } else if (today[0]) {
    snprintf(statusBuf, sizeof(statusBuf), "会话 %u · 今日 %s", (unsigned)st.sess, today);
  } else {
    snprintf(statusBuf, sizeof(statusBuf), "会话 %u · 待命中", (unsigned)st.sess);
  }
  ui::drawTextCentered(c, ui::fontCjk14(), (int16_t)(UI_W / 2 + ox), (int16_t)(438 + oy),
                       statusBuf, ui::COL_MUTED, ui::COL_BG);
}

UiAction pageHomeTap(int16_t x, int16_t y, const NovaState& st) {
  if (st.wifiApMode && !st.wifiConnected && apCancelPortalRect(st).contains(x, y)) {
    return UiAction{ UiAction::CANCEL_PORTAL, 0 };
  }

  // 详情浮层开着时，点哪都先收起，避免误触到舞台/胶囊。
  if (st.dockExpanded && st.turnError && st.errMsg[0]) {
    return UiAction{ UiAction::DOCK_TOGGLE, 0 };
  }
  // 异常时点码头展开详情；非异常时码头不拦点击，保持原有舞台交互。
  if (st.turnError && dockRect(st).contains(x, y)) {
    return UiAction{ UiAction::DOCK_TOGGLE, 0 };
  }

  const ui::Rect pillR = statusPillRect(st);
  if (pillR.contains(x, y)) {
    if (s_demoAnim < 0) s_demoAnim = 1;
    else if (s_demoAnim >= (int)NvaAnimKey::COUNT - 1) s_demoAnim = -1;
    else s_demoAnim++;
    setBubble((s_demoAnim < 0) ? "恢复自动状态" : "切换动作!", 3500u, st.nowMs);
    return UiAction{ UiAction::PET_TAP, 2 };
  }

  if (y >= STAGE_TOP && y <= STAGE_BOTTOM) {
    const int16_t ox = ui::burnOffsetX(st.burnInStep);
    const int16_t oy = ui::burnOffsetY(st.burnInStep);
    int16_t curX = (int16_t)(s_petX + 0.5f) + ox;
    int16_t dx = x - curX;
    if (dx < 0) dx = -dx;
    if (dx <= 68 && y >= (int16_t)(104 + oy)) {
      s_sparkFrames = 24;
      s_pendingSquashTap = true;
      const NvaAnimKey cur = nvaActiveState();
      if (cur == NvaAnimKey::SLEEP || cur == NvaAnimKey::YAWN) {
        s_comboCount = 0;
        const int wakeQ = s_quoteIdx % 2;
        setBubble(kWakeQuotes[wakeQ], 5500u, st.nowMs);
        s_quoteIdx++;
        return UiAction{ UiAction::PET_TAP, 0, (int)NvaAnimKey::WAKE_UP, wakeQ };
      }

      uint8_t kept = 0;
      for (uint8_t i = 0; i < s_comboCount; i++) {
        if (st.nowMs >= s_comboTimes[i] && (st.nowMs - s_comboTimes[i]) <= 8000u) {
          s_comboTimes[kept++] = s_comboTimes[i];
        }
      }
      s_comboCount = kept;
      if (s_comboCount >= 2) {
        s_comboCount = 0;
        const int angryQ = s_quoteIdx % 2;
        setBubble(kClickAngryQuotes[angryQ], 6000u, st.nowMs);
        s_quoteIdx++;
        return UiAction{ UiAction::PET_TAP, 0, (int)NvaAnimKey::CLICK_ANGRY, angryQ };
      }

      s_comboTimes[s_comboCount++] = st.nowMs;
      static constexpr NvaAnimKey kClickPool[4] = {
        NvaAnimKey::CLICK,
        NvaAnimKey::CLICK_HAPPY,
        NvaAnimKey::CLICK_SHY,
        NvaAnimKey::CLICK_GIGGLE,
      };
      s_clickPoolIdx = (uint8_t)((s_clickPoolIdx + 1) % 4);
      if (kClickPool[s_clickPoolIdx] == s_lastClickKey || kClickPool[s_clickPoolIdx] == cur) {
        s_clickPoolIdx = (uint8_t)((s_clickPoolIdx + 1) % 4);
      }
      if (kClickPool[s_clickPoolIdx] == s_lastClickKey || kClickPool[s_clickPoolIdx] == cur) {
        s_clickPoolIdx = (uint8_t)((s_clickPoolIdx + 1) % 4);
      }
      const NvaAnimKey chosen = kClickPool[s_clickPoolIdx];
      s_lastClickKey = chosen;
      const int clickQ = s_quoteIdx % 2;
      const char* quote = kClickWaveQuotes[clickQ];
      if (chosen == NvaAnimKey::CLICK_HAPPY) quote = kClickHappyQuotes[clickQ];
      else if (chosen == NvaAnimKey::CLICK_SHY) quote = kClickShyQuotes[clickQ];
      else if (chosen == NvaAnimKey::CLICK_GIGGLE) quote = kClickGiggleQuotes[clickQ];
      s_quoteIdx++;
      setBubble(quote, 6500u, st.nowMs);
      return UiAction{ UiAction::PET_TAP, 0, (int)chosen, clickQ };
    }
    int16_t tx = (int16_t)(x - ox);
    if (tx < PET_MIN_X) tx = PET_MIN_X;
    if (tx > PET_MAX_X) tx = PET_MAX_X;
    s_targetX = (float)tx;
    s_walkLeadUntilMs = st.nowMs;
    s_runSpeed = true;
    s_userTargetLock = true;
    s_nextRoamMs = st.nowMs + 14000u;
    setBubble("跑过来啦~", 4500u, st.nowMs);
    return UiAction{ UiAction::PET_TAP, 1 };
  }

  return UiAction{};
}

bool pageHomeHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings&, ui::Rect* out) {
  if (!out) return false;
  if (st.wifiApMode && !st.wifiConnected && apCancelPortalRect(st).contains(x, y)) {
    *out = apCancelPortalRect(st);
    return true;
  }
  const ui::Rect pillR = statusPillRect(st);
  if (pillR.contains(x, y)) {
    *out = pillR;
    return true;
  }
  return false;
}
