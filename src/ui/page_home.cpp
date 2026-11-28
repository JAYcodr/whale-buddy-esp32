#include "qrcode.h"
#include "ui/pages.h"
#include "anim/nva.h"
#include "ui/theme.h"
#include "ui/tide_palette.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NOVA_HOST_SIM
#include <esp_attr.h>
#include <esp_heap_caps.h>
#endif

void drawDockDetail(NovaCanvas& c, const NovaState& st);

namespace {

constexpr ui::Rect BTN_CANCEL_PORTAL{ 94, 384, 180, 44 };  // AP 页重排后按钮下移，不压二维码
constexpr int16_t STAGE_TOP    = 62;
constexpr int16_t STAGE_BOTTOM = 304;
constexpr int16_t PET_MIN_X    = 80;
constexpr int16_t PET_MAX_X    = 288;
constexpr int16_t PET_BASE_Y   = 298;

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
bool     s_drainHold        = false;
uint8_t  s_continuousMove   = 0;   // 0=none, 1=crab_walk, 2=happy_run

bool isLocomotionAnim(int animIdx) {
  return animIdx == (int)NvaAnimKey::CRAB_WALK ||
         animIdx == (int)NvaAnimKey::HAPPY_RUN;
}

float bounceOppositeTarget(float curX, uint32_t seed) {
  const float span = (float)((seed * 29u) % 61u);
  if (curX >= 184.0f) {
    return 92.0f + span;   // [92..152]
  }
  return 216.0f + span;    // [216..276]
}

#ifndef NOVA_HOST_SIM
RTC_DATA_ATTR uint32_t s_seenChoreoSig = 0;
RTC_DATA_ATTR uint8_t  s_choreoIdx     = 0;
#else
uint32_t s_seenChoreoSig    = 0;
uint8_t  s_choreoIdx        = 0;
#endif
uint32_t s_seenChoreoGen    = 0;
bool     s_choreoArrived    = true;
bool     s_choreoStepInit   = false;
uint32_t s_choreoStepEnd    = 0;

uint32_t choreoSignature(const NovaState& st) {
  uint32_t h = 2166136261u;
  auto mixBytes = [&](const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 16777619u;
    }
  };
  mixBytes(&st.nChoreoSteps, 1);
  mixBytes(st.choreoMood, strnlen(st.choreoMood, sizeof(st.choreoMood)));
  for (uint8_t i = 0; i < st.nChoreoSteps && i < MAX_CHOREO_STEPS; i++) {
    const ChoreoStep& s = st.choreoSteps[i];
    mixBytes(&s.x, sizeof(s.x));
    mixBytes(&s.run, sizeof(s.run));
    mixBytes(&s.anim, sizeof(s.anim));
    mixBytes(&s.durMs, sizeof(s.durMs));
    mixBytes(s.say, strnlen(s.say, sizeof(s.say)));
  }
  return h;
}

const char* demoTagLabel(int idx) {
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
  const int safe = (idx >= 0) ? (idx % (int)NvaAnimKey::COUNT) : 0;
  return kDemoTags[safe];
}
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
ui::Rect s_prevBubbleRect{ 0, 0, 0, 0 };
bool     s_prevHadBubble = false;

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
  if (s_petX < (float)PET_MIN_X) s_petX = (float)PET_MIN_X;
  if (s_petX > (float)PET_MAX_X) s_petX = (float)PET_MAX_X;

  if (st.choreoGen != s_seenChoreoGen) {
    s_seenChoreoGen = st.choreoGen;
    const uint32_t sig = choreoSignature(st);
    if (sig != s_seenChoreoSig) {
      s_seenChoreoSig = sig;
      s_choreoIdx = 0;
      s_choreoArrived = false;
      s_choreoStepInit = false;
      s_choreoStepEnd = 0;
    } else if (st.nChoreoSteps > 0 && s_choreoIdx >= st.nChoreoSteps) {
      s_choreoIdx = 0;
    }
  }

  // Execute LLM choreography script when enabled and in autonomous idle/patrol mode.
  // 犯困/熟睡期间（YAWN / SLEEP / AOD）必须暂停剧本推步，
  // 否则下一步 step.x / step.say 会把熟睡中的诺瓦拉起来“梦游螃蟹步”并顶着 Zzz 弹元气台词。
  if (cfg.choreoEnabled && st.nChoreoSteps > 0 && s_demoAnim < 0 && st.run == 0 && st.wait == 0 &&
      !st.approvalActive && !s_userTargetLock && !resting) {
    if (!s_choreoStepInit) {
      if (s_drainHold) {
        s_choreoAnim = -1;
        return;
      }
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

    if (!s_drainHold && s_choreoArrived && (int32_t)(st.nowMs - s_choreoStepEnd) >= 0) {
      bool cycleComplete = (nvaLoopCycles() > s_choreoStartCycle);
      bool safetyExpired = ((int32_t)(st.nowMs - (s_choreoStepEnd + 5000u)) >= 0);
      if (cycleComplete || safetyExpired) {
        s_choreoIdx = (uint8_t)((s_choreoIdx + 1) % st.nChoreoSteps);
        s_choreoStepInit = false;
      }
    } else if (!s_drainHold && s_choreoArrived && isLocomotionAnim(s_choreoAnim) &&
               fabsf(s_targetX - s_petX) <= 1.2f) {
      // 跑步/螃蟹步到达目标或屏幕边缘时自动折返换向，避免原地贴墙跑。
      s_targetX = bounceOppositeTarget(s_petX, st.nowMs + frame);
      s_runSpeed = (s_choreoAnim == (int)NvaAnimKey::HAPPY_RUN);
      s_walkLeadUntilMs = 0;
    }
    return;
  }
  s_choreoAnim = -1;
  s_choreoStepInit = false;

  // 外部状态机主动播放跑步/螃蟹步（工作小插曲、Demo 预览）时，到边缘自动折返。
  if (s_continuousMove > 0 && !resting && !s_drainHold && !s_userTargetLock &&
      !st.approvalActive && st.wait == 0) {
    if (fabsf(s_targetX - s_petX) <= 1.2f) {
      s_targetX = bounceOppositeTarget(s_petX, st.nowMs + frame);
      s_runSpeed = (s_continuousMove == 2);
      s_walkLeadUntilMs = 0;
    }
    return;
  }

  // Built-in local fallback patrol when no LLM script is active.
  if (s_petX == s_targetX && (int32_t)(st.nowMs - s_nextRoamMs) >= 0 && st.wait == 0 && !st.approvalActive) {
    if (resting || s_drainHold) {
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
    dotCol = ui::COL_VIOLET;
    copyUtf8Safe(tagBuf, sizeof(tagBuf), demoTagLabel(s_demoAnim));
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

// ── 潮汐（TIDE）主屏 ──────────────────────────────────────────────────
// 契约见 docs/design/tide-theme-task-card.md §4①：状态淹没底色 + 92px 巨字时间背景
// + 诺瓦前景 + 通栏码头。原版 OCEAN 的绘制一像素不动，两者各走各的分支。
constexpr int16_t TIDE_STRIP_H      = 38;
constexpr int16_t TIDE_STAGE_BOTTOM = 338;
constexpr int16_t TIDE_PET_BASE_Y   = 334;  // 脚底轻踩在底栏横线 342 上方 8px
constexpr int16_t TIDE_GIANT_TOP    = 46;   // 1.45× 巨字时间紧贴顶栏下方（高 ~98px → 46..144）
constexpr int16_t TIDE_EYEBROW_Y    = 168;  // 位于巨字时间正下方左侧，严格对齐设计稿构图
constexpr int16_t TIDE_BUBBLE_Y     = 148;  // 与眉标同高右侧对称
constexpr int16_t TIDE_DOCK_TOP     = 342;
constexpr float   TIDE_GIANT_SCALE  = 1.44f;

inline void tideHomePalette(const NovaState& st, uint16_t* bg, uint16_t* giantCol) {
  if (st.approvalActive || st.turnError || st.wait > 0) {
    *bg = ui::tide::BG_CRIMSON;
    *giantCol = ui::tide::GIANT_ALERT;
  } else if (st.run > 0) {
    *bg = ui::tide::BG_COBALT;
    *giantCol = ui::tide::GIANT_WORK;
  } else {
    *bg = ui::tide::BG_NIGHT;
    *giantCol = ui::tide::GIANT_IDLE;
  }
}

inline const char* tideEyebrow(const NovaState& st) {
  if (s_demoAnim >= 0) {
    static char s_demoEyebrow[48];
    snprintf(s_demoEyebrow, sizeof(s_demoEyebrow), "DEMO · %s", demoTagLabel(s_demoAnim));
    return s_demoEyebrow;
  }
  if (st.approvalActive || st.turnError || st.wait > 0) return "ALERT · 异常";
  if (st.run > 0) return "ACTIVE · 运行中";
  return "STANDBY · 摸鱼中";
}

// 双线性放大绘制 4-bpp 单字形（仅用于主屏 1.44× 巨字背景时间，零新增字库）。
const AaGlyph* findDigitGlyph92(uint16_t cp) {
  const AaFont* f = ui::fontNum92();
  if (!f) return nullptr;
  for (uint16_t i = 0; i < f->glyphCount; i++) {
    if (f->glyphs[i].cp == cp) return &f->glyphs[i];
  }
  return nullptr;
}

void drawScaledGlyph92(NovaCanvas& c, const AaGlyph* g, int16_t dstX, int16_t dstTopY,
                       float scale, uint16_t col) {
  if (!g || g->w == 0 || g->h == 0) return;
  uint16_t* fb = c.getFramebuffer();
  if (!fb) return;
  const AaFont* f = ui::fontNum92();
  const uint8_t* data = f->data + g->dataOff;
  const int16_t W = (int16_t)c.width();
  const int16_t H = (int16_t)c.height();
  const int16_t dw = (int16_t)lroundf((float)g->w * scale);
  const int16_t dh = (int16_t)lroundf((float)g->h * scale);
  const float invS = 1.0f / scale;

  auto sampleA = [&](int sx, int sy) -> float {
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= (int)g->w) sx = (int)g->w - 1;
    if (sy >= (int)g->h) sy = (int)g->h - 1;
    const uint32_t pi = (uint32_t)sy * g->w + (uint32_t)sx;
    const uint8_t byte = data[pi >> 1];
    return (float)((pi & 1u) ? (byte & 0x0Fu) : (byte >> 4));
  };

  for (int16_t dy = 0; dy < dh; dy++) {
    const int16_t py = (int16_t)(dstTopY + dy);
    if (py < TIDE_STRIP_H || py >= TIDE_STAGE_BOTTOM || py < 0 || py >= H) continue;
    const float fy = ((float)dy + 0.5f) * invS - 0.5f;
    const int sy0 = (int)floorf(fy);
    const float ty = fy - (float)sy0;
    uint16_t* row = fb + (int32_t)py * W;

    for (int16_t dx = 0; dx < dw; dx++) {
      const int16_t px = (int16_t)(dstX + dx);
      if (px < 0 || px >= W) continue;
      const float fx = ((float)dx + 0.5f) * invS - 0.5f;
      const int sx0 = (int)floorf(fx);
      const float tx = fx - (float)sx0;
      const float a00 = sampleA(sx0, sy0);
      const float a10 = sampleA(sx0 + 1, sy0);
      const float a01 = sampleA(sx0, sy0 + 1);
      const float a11 = sampleA(sx0 + 1, sy0 + 1);
      const float a = (a00 * (1.0f - tx) + a10 * tx) * (1.0f - ty) +
                      (a01 * (1.0f - tx) + a11 * tx) * ty;
      const uint8_t alpha255 = (uint8_t)lroundf(a * (255.0f / 15.0f));
      if (alpha255 > 4) {
        row[px] = ui::blend565(row[px], col, alpha255);
      }
    }
  }
}

struct TideClockLayout {
  const AaGlyph* g[4];
  int16_t x[4];
  int16_t colonX;
  int16_t topY;
  int16_t h;
  int16_t x0, x1;
};

TideClockLayout layoutTideGiantClock(const NovaState& st, int16_t oy) {
  TideClockLayout L{};
  const uint16_t cps[4] = {
    (uint16_t)('0' + (st.hour / 10) % 10),
    (uint16_t)('0' + st.hour % 10),
    (uint16_t)('0' + (st.minute / 10) % 10),
    (uint16_t)('0' + st.minute % 10),
  };
  int16_t w[4] = {};
  int16_t maxH = 0;
  for (int i = 0; i < 4; i++) {
    L.g[i] = findDigitGlyph92(cps[i]);
    w[i] = L.g[i] ? (int16_t)lroundf((float)L.g[i]->w * TIDE_GIANT_SCALE) : 60;
    int16_t gh = L.g[i] ? (int16_t)lroundf((float)L.g[i]->h * TIDE_GIANT_SCALE) : 96;
    if (gh > maxH) maxH = gh;
  }
  constexpr int16_t kDigitGap = 5;
  constexpr int16_t kColonGap = 32;
  const int16_t totalW = (int16_t)(w[0] + kDigitGap + w[1] + kColonGap + w[2] + kDigitGap + w[3]);
  const int16_t startX = (int16_t)((UI_W - totalW) / 2);
  L.x[0] = startX;
  L.x[1] = (int16_t)(L.x[0] + w[0] + kDigitGap);
  L.colonX = (int16_t)(L.x[1] + w[1] + kColonGap / 2);
  L.x[2] = (int16_t)(L.x[1] + w[1] + kColonGap);
  L.x[3] = (int16_t)(L.x[2] + w[2] + kDigitGap);
  L.topY = (int16_t)(TIDE_GIANT_TOP + oy);
  L.h = maxH;
  L.x0 = (int16_t)(startX - 2);
  L.x1 = (int16_t)(startX + totalW + 2);
  return L;
}

// 1.44× 巨字时间的着墨盒（含 AA 余量 + 矢量方冒号）。
ui::Rect tideGiantClockBox(NovaCanvas&, const NovaState& st, int16_t oy) {
  const TideClockLayout L = layoutTideGiantClock(st, oy);
  return ui::Rect{ L.x0, (int16_t)(L.topY - 2),
                   (int16_t)(L.x1 - L.x0), (int16_t)(L.h + 4) };
}

// 1.44× 巨字时间（紧贴顶栏下方、横跨近 300px，复刻设计稿封面级张力）。
void drawTideGiantClock(NovaCanvas& c, const NovaState& st, int16_t oy, uint16_t col) {
  const TideClockLayout L = layoutTideGiantClock(st, oy);
  for (int i = 0; i < 4; i++) {
    drawScaledGlyph92(c, L.g[i], L.x[i], L.topY, TIDE_GIANT_SCALE, col);
  }
  // 几何方冒号（对齐设计稿粗重 DIN 气质）
  c.fillRect((int16_t)(L.colonX - 6), (int16_t)(L.topY + 26), 12, 12, col);
  c.fillRect((int16_t)(L.colonX - 6), (int16_t)(L.topY + 62), 12, 12, col);
}

ui::Rect tideEyebrowRect(NovaCanvas& c, const NovaState& st, int16_t oy) {
  const char* tag = tideEyebrow(st);
  ui::TextMetrics m = ui::measureText(c, ui::fontCjk14(), tag);
  const int16_t baseY = (int16_t)(TIDE_EYEBROW_Y + oy);
  return ui::Rect{ (int16_t)(UI_SAFE_L + 2 - 1), (int16_t)(baseY + m.y1 - 1),
                   (int16_t)(m.w + 2), (int16_t)(m.h + 2) };
}

inline bool rectsIntersect(const ui::Rect& a, const ui::Rect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

inline void rectUnion(ui::Rect& a, const ui::Rect& b) {
  const int16_t x1 = (int16_t)(a.x + a.w < b.x + b.w ? b.x + b.w : a.x + a.w);
  const int16_t y1 = (int16_t)(a.y + a.h < b.y + b.h ? b.y + b.h : a.y + a.h);
  const int16_t x0 = a.x < b.x ? a.x : b.x;
  const int16_t y0 = a.y < b.y ? a.y : b.y;
  a = ui::Rect{ x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

constexpr int16_t TIDE_STAGE_H = TIDE_STAGE_BOTTOM - TIDE_STRIP_H;
uint16_t* s_tideBgBuf = nullptr;
bool      s_tideBgAllocTried = false;
bool      s_tideBgValid      = false;
uint16_t  s_tideBgCol        = 0;
uint16_t  s_tideBgGiantCol   = 0;
uint8_t   s_tideBgHour       = 255;
uint8_t   s_tideBgMinute     = 255;
uint8_t   s_tideBgBurnStep   = 255;
uint8_t   s_tideBgStateKind  = 255;
int       s_tideBgDemoAnim   = -1;

uint16_t* ensureTideBgBuf() {
  if (s_tideBgBuf) return s_tideBgBuf;
  if (s_tideBgAllocTried) return nullptr;
  s_tideBgAllocTried = true;
  const size_t bytes = (size_t)UI_W * (size_t)TIDE_STAGE_H * sizeof(uint16_t);
#ifdef NOVA_HOST_SIM
  s_tideBgBuf = (uint16_t*)malloc(bytes);
#else
  s_tideBgBuf = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  return s_tideBgBuf;
}

inline uint8_t tideStateKind(const NovaState& st) {
  if (st.approvalActive || st.turnError || st.wait > 0) return 2;
  if (st.run > 0) return 1;
  return 0;
}

inline bool tideBgMatches(const NovaState& st, uint16_t bg, uint16_t giantCol) {
  return s_tideBgValid &&
         s_tideBgCol == bg &&
         s_tideBgGiantCol == giantCol &&
         s_tideBgHour == st.hour &&
         s_tideBgMinute == st.minute &&
         s_tideBgBurnStep == st.burnInStep &&
         s_tideBgStateKind == tideStateKind(st) &&
         s_tideBgDemoAnim == s_demoAnim;
}

void drawTideStageStatic(NovaCanvas& c, const NovaState& st, int16_t oy,
                         uint16_t bg, uint16_t giantCol) {
  c.fillRect(0, TIDE_STRIP_H, UI_W, TIDE_STAGE_H, bg);
  drawTideGiantClock(c, st, oy, giantCol);
  ui::drawText(c, ui::fontCjk14(), UI_SAFE_L + 2, (int16_t)(TIDE_EYEBROW_Y + oy),
               tideEyebrow(st), ui::tide::dimOn(bg), bg);
}

void snapshotTideBg(NovaCanvas& c) {
  uint16_t* dst = ensureTideBgBuf();
  const uint16_t* fb = c.getFramebuffer();
  if (!dst || !fb) return;
  const uint16_t* src = fb + (int32_t)TIDE_STRIP_H * (int16_t)c.width();
  memcpy(dst, src, (size_t)UI_W * (size_t)TIDE_STAGE_H * sizeof(uint16_t));
}

bool restoreTideBgRect(NovaCanvas& c, const ui::Rect& r) {
  if (!s_tideBgBuf || !s_tideBgValid) return false;
  uint16_t* fb = c.getFramebuffer();
  if (!fb) return false;
  const int16_t W = (int16_t)c.width();
  int16_t x0 = r.x;
  int16_t y0 = r.y;
  int16_t x1 = (int16_t)(r.x + r.w);
  int16_t y1 = (int16_t)(r.y + r.h);
  if (x0 < 0) x0 = 0;
  if (x1 > W) x1 = W;
  if (y0 < TIDE_STRIP_H) y0 = TIDE_STRIP_H;
  if (y1 > TIDE_STAGE_BOTTOM) y1 = TIDE_STAGE_BOTTOM;
  if (x1 <= x0 || y1 <= y0) return true;
  const size_t rowBytes = (size_t)(x1 - x0) * sizeof(uint16_t);
  for (int16_t y = y0; y < y1; y++) {
    const uint16_t* srcRow = s_tideBgBuf + (int32_t)(y - TIDE_STRIP_H) * UI_W + x0;
    uint16_t* dstRow = fb + (int32_t)y * W + x0;
    memcpy(dstRow, srcRow, rowBytes);
  }
  return true;
}

void drawStageContentTide(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg,
                          uint32_t frame, int16_t* outDx, int16_t* outDy,
                          int16_t* outDw, int16_t* outDh) {
  uint16_t bg, giantCol;
  tideHomePalette(st, &bg, &giantCol);

  updateRoam(st, cfg, frame);

  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const int16_t cx = (int16_t)(s_petX + 0.5f) + ox;

  float diff = s_targetX - s_petX;
  const bool walking = (diff < -1.2f || diff > 1.2f);
  const int16_t hop = walking ? (int16_t)(fabsf(sinf(frame * 0.28f)) * 2.0f) : 0;
  const int16_t bottomY = (int16_t)(TIDE_PET_BASE_Y + oy - hop);

  const int16_t sprW = nvaFrameWidth();
  const int16_t sprH = nvaFrameHeight();
  // 常规态保持贴身包围盒（max sprW=213 → dw<=224 < 240），让每帧都留在 64KB 内部 DMA SRAM 快传路径；
  // 仅在点击弹跳的 ~280ms 期间按 squash 最大倍率放大包围盒，避免静态也把宽图推过 240 掉入慢路径。
  int16_t halfW = s_squashActive ? (int16_t)(lroundf((float)sprW * 0.58f) + 4) : (int16_t)(sprW / 2 + 4);
  if (halfW < 84) halfW = 84;
  int16_t curX0 = (int16_t)(cx - halfW);
  int16_t curX1 = (int16_t)(cx + halfW);
  const int16_t upH = s_squashActive ? (int16_t)lroundf((float)sprH * 1.14f) : sprH;
  int16_t curY0 = (int16_t)(bottomY - upH - 4);
  int16_t curY1 = (int16_t)(TIDE_PET_BASE_Y + oy + 6);

  // 气泡固定贴右上（y = 160），不跟着诺瓦跑到脸上。
  bool showBubble = (s_bubbleBuf[0] && s_bubbleUntilMs != 0 &&
                     (int32_t)(s_bubbleUntilMs - st.nowMs) > 0);
  if (s_bubbleBuf[0] && !showBubble && s_bubbleUntilMs != 0) {
    s_bubbleBuf[0] = '\0';
    s_bubbleUntilMs = 0;
  }
  constexpr int16_t kBubbleH = 30;
  int16_t bx = 12, by = (int16_t)(TIDE_BUBBLE_Y + oy), bw = 0;
  if (showBubble) {
    ui::TextMetrics bm = ui::measureText(c, ui::fontCjk14(), s_bubbleBuf);
    bw = (int16_t)(bm.w + 26);
    if (bw > UI_W - 24) bw = UI_W - 24;
    bx = (int16_t)(UI_SAFE_R - bw);
    if (bx < 12) bx = 12;
    if (by < TIDE_STRIP_H + 4) by = TIDE_STRIP_H + 4;
    if (by + kBubbleH > TIDE_STAGE_BOTTOM - 4) by = (int16_t)(TIDE_STAGE_BOTTOM - 4 - kBubbleH);
  }

  int16_t uX0 = curX0, uY0 = curY0, uX1 = curX1, uY1 = curY1;
  if (s_hasPrevBox) {
    if (s_prevX0 < uX0) uX0 = s_prevX0;
    if (s_prevY0 < uY0) uY0 = s_prevY0;
    if (s_prevX1 > uX1) uX1 = s_prevX1;
    if (s_prevY1 > uY1) uY1 = s_prevY1;
  } else {
    uX0 = 0; uY0 = TIDE_STRIP_H; uX1 = UI_W; uY1 = TIDE_STAGE_BOTTOM;
  }

  s_prevX0 = curX0; s_prevY0 = curY0; s_prevX1 = curX1; s_prevY1 = curY1;
  s_hasPrevBox = true;

  ui::Rect dirty{ uX0, uY0, (int16_t)(uX1 - uX0), (int16_t)(uY1 - uY0) };
  if (s_prevHadBubble) {
    rectUnion(dirty, s_prevBubbleRect);
  }
  if (showBubble) {
    const ui::Rect bubbleRect{ (int16_t)(bx - 2), (int16_t)(by - 2),
                               (int16_t)(bw + 4), (int16_t)(kBubbleH + 4) };
    rectUnion(dirty, bubbleRect);
    s_prevBubbleRect = bubbleRect;
    s_prevHadBubble = true;
  } else {
    s_prevHadBubble = false;
  }

  // 舞台背景（底色 + 1.44× 巨字时间 + 眉标）只随分钟 / 主题调色板 / 烧屏位移变化，
  // 缓存在 PSRAM 中后每帧只需按宠物脏矩形 memcpy 回填，不必重算双线性缩放，也避免把脏区扩成 >=240px 掉出快传路径。
  const bool bgCached = (ensureTideBgBuf() != nullptr);
  const bool bgHit = bgCached && tideBgMatches(st, bg, giantCol);
  if (bgCached) {
    if (!bgHit) {
      dirty = ui::Rect{ 0, TIDE_STRIP_H, UI_W, TIDE_STAGE_H };
    }
  } else {
    const ui::Rect giantBox = tideGiantClockBox(c, st, oy);
    const ui::Rect eyebrowBox = tideEyebrowRect(c, st, oy);
    const bool drawGiant = rectsIntersect(dirty, giantBox);
    const bool drawEyebrow = rectsIntersect(dirty, eyebrowBox);
    if (drawGiant) rectUnion(dirty, giantBox);
    if (drawEyebrow) rectUnion(dirty, eyebrowBox);
    dirty = ui::Rect{ dirty.x, dirty.y, dirty.w, dirty.h };
  }

  // 裁剪到舞台区，绝不越界到顶栏或码头。
  int16_t x0 = dirty.x;
  int16_t y0 = dirty.y;
  int16_t x1 = (int16_t)(dirty.x + dirty.w);
  int16_t y1 = (int16_t)(dirty.y + dirty.h);
  if (x0 < 0) x0 = 0;
  if (x1 > UI_W) x1 = UI_W;
  if (y0 < TIDE_STRIP_H) y0 = TIDE_STRIP_H;
  if (y1 > TIDE_STAGE_BOTTOM) y1 = TIDE_STAGE_BOTTOM;
  if (x1 <= x0 || y1 <= y0) {
    if (outDx) { *outDx = 0; *outDy = TIDE_STRIP_H; *outDw = 1; *outDh = 1; }
    return;
  }
  dirty = ui::Rect{ x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };

  if (bgCached) {
    if (!bgHit) {
      drawTideStageStatic(c, st, oy, bg, giantCol);
      snapshotTideBg(c);
      s_tideBgCol = bg;
      s_tideBgGiantCol = giantCol;
      s_tideBgHour = st.hour;
      s_tideBgMinute = st.minute;
      s_tideBgBurnStep = st.burnInStep;
      s_tideBgStateKind = tideStateKind(st);
      s_tideBgDemoAnim = s_demoAnim;
      s_tideBgValid = true;
    } else {
      restoreTideBgRect(c, dirty);
    }
  } else {
    const ui::Rect giantBox = tideGiantClockBox(c, st, oy);
    const ui::Rect eyebrowBox = tideEyebrowRect(c, st, oy);
    c.fillRect(dirty.x, dirty.y, dirty.w, dirty.h, bg);
    if (rectsIntersect(dirty, giantBox)) drawTideGiantClock(c, st, oy, giantCol);
    if (rectsIntersect(dirty, eyebrowBox)) {
      ui::drawText(c, ui::fontCjk14(), UI_SAFE_L + 2, (int16_t)(TIDE_EYEBROW_Y + oy),
                   tideEyebrow(st), ui::tide::dimOn(bg), bg);
    }
  }

  if (!nvaDraw(c, cx, bottomY, s_facingRight && spriteAllowsMirror(nvaActiveState()),
               calcSquashScaleY(st.nowMs))) {
    ui::aaFillCircle(c, cx, (int16_t)(bottomY - 76), 36,
                     (bg == ui::tide::BG_NIGHT) ? ui::tide::SURF_SUB : ui::tide::WHITE);
  }

  if (showBubble) {
    ui::aaFillRoundRect(c, bx, by, bw, kBubbleH, 6, ui::tide::WHITE, ui::tide::WHITE);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), { bx, by, bw, kBubbleH },
                               s_bubbleBuf, ui::tide::SURF_INK, ui::tide::WHITE);
  }

  if (outDx) *outDx = dirty.x;
  if (outDy) *outDy = dirty.y;
  if (outDw) *outDw = dirty.w;
  if (outDh) *outDh = dirty.h;
}

// 底栏码头：顶部 2px 细横线 + 左侧纯白反色块（RUN N · CTX XX%）+ 右侧模型名
// + 下方一行纯白状态文本。通栏无卡片、零圆角。
void drawTideDock(NovaCanvas& c, const NovaState& st, uint16_t bg) {
  c.fillRect(0, TIDE_DOCK_TOP, UI_W, UI_H - TIDE_DOCK_TOP, bg);
  c.fillRect(0, TIDE_DOCK_TOP, UI_W, 2, ui::blend565(bg, ui::tide::WHITE, 95));

  char runBuf[40];
  if (st.ctxPct >= 0) {
    if (st.turnError) snprintf(runBuf, sizeof(runBuf), "ERR · CTX %d%%", st.ctxPct);
    else snprintf(runBuf, sizeof(runBuf), "RUN %u · CTX %d%%", (unsigned)st.run, st.ctxPct);
  } else {
    if (st.turnError) snprintf(runBuf, sizeof(runBuf), "ERR");
    else snprintf(runBuf, sizeof(runBuf), "RUN %u", (unsigned)st.run);
  }

  constexpr int16_t kBlkY = 354;
  constexpr int16_t kBlkH = 30;
  // 用 CJK 字库画整串：Din17 的 U+00B7 字形是坏的（渲染成空心方块），
  // 而本行文案里带「·」，原版同样走 fontCjk14。
  ui::TextMetrics rm = ui::measureText(c, ui::fontCjk14(), runBuf);
  const int16_t blkW = (int16_t)(rm.w + 20);
  const int16_t blkX = UI_SAFE_L;
  c.fillRect(blkX, kBlkY, blkW, kBlkH, ui::tide::WHITE);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), { blkX, kBlkY, blkW, kBlkH },
                             runBuf, bg, ui::tide::WHITE);

  if (st.activeModel[0]) {
    const int16_t avail = (int16_t)(UI_SAFE_R - (blkX + blkW) - 12);
    if (avail > 40) {
      char modelBuf[48];
      ui::fitUtf8Line(c, ui::fontCjk14(), st.activeModel, avail, modelBuf, sizeof(modelBuf));
      ui::drawTextRight(c, ui::fontCjk14(), UI_SAFE_R, kBlkY + 21, modelBuf,
                        ui::tide::dimOn(bg), bg);
    }
  }

  const char* line = st.msg[0] ? st.msg
                     : (st.sessTitle[0] ? st.sessTitle
                        : (st.run == 0 ? "待机摸鱼中 · 轻戳诺瓦互动" : "任务执行中"));
  char lineBuf[96];
  ui::fitUtf8Line(c, ui::fontCjk16(), line, UI_SAFE_W, lineBuf, sizeof(lineBuf));
  ui::drawText(c, ui::fontCjk16(), UI_SAFE_L, 416, lineBuf, ui::tide::WHITE, bg);
}

void drawHomeTide(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame) {
  uint16_t bg, giantCol;
  tideHomePalette(st, &bg, &giantCol);
  c.fillScreen(bg);
  const char* topLeft = (st.wifiConnected && st.mqttConnected) ? "DSH" : "OFFLINE";
  ui::tide::drawTopStrip(c, st, 1, bg, 6, topLeft);

  s_hasPrevBox = false;
  s_prevHadBubble = false;
  drawStageContentTide(c, st, cfg, frame, nullptr, nullptr, nullptr, nullptr);
  drawTideDock(c, st, bg);
  if (st.dockExpanded && st.turnError && st.errMsg[0]) {
    drawDockDetail(c, st);
  }
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
    "edit", "write", "patch", "bash", "terminal", "cmd", "command",
    "shell", "exec", "create", "replace", "code", "apply",
  };
  for (const char* kw : kCodingTools) {
    if (containsCaseInsensitive(tool, kw)) return NvaAnimKey::CODING;
  }
  static constexpr const char* const kArchiveTools[] = {
    "read", "grep", "glob", "search", "find", "list", "fetch",
    "view", "cat", "ls", "dir", "outline", "inspect", "web",
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

void pageHomeSetDrainHold(bool hold) {
  s_drainHold = hold;
}

void pageHomeSetContinuousMove(uint8_t mode) {
  s_continuousMove = mode;
}

void pageHomeBookmarkAdvance(const NovaState& st, bool advanceChoreo) {
  if (advanceChoreo && st.nChoreoSteps > 0 && s_choreoStepInit) {
    s_choreoIdx = (uint8_t)((s_choreoIdx + 1) % st.nChoreoSteps);
  }
  s_choreoStepInit = false;
  s_choreoAnim = -1;
  s_targetX = s_petX;
  s_runSpeed = false;
  s_userTargetLock = false;
}

uint8_t pageHomeChoreoIdx() {
  return s_choreoIdx;
}

void pageHomeShowBubble(const char* txt, uint32_t durMs, uint32_t nowMs) {
  setBubble(txt, durMs, nowMs);
}

void pageHomeTick(const NovaState& st, const NovaSettings& cfg, uint32_t frame, bool idleResting) {
  s_idleResting = idleResting;
  updateRoam(st, cfg, frame);
}

void pageHomeDrawStage(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame,
                       int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH) {
  if (ui::isTideTheme()) {
    drawStageContentTide(c, st, cfg, frame, outX, outY, outW, outH);
    return;
  }
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
  // AP 配网态在两种主题下都走原版绘制（契约要求 AP 二维码保持原样不动像素）。
  if (ui::isTideTheme() && !(st.wifiApMode && !st.wifiConnected)) {
    drawHomeTide(c, st, cfg, frame);
    return;
  }

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

int16_t s_aodPrevX0 = 100, s_aodPrevY0 = 230, s_aodPrevX1 = 268, s_aodPrevY1 = 420;
bool    s_aodHasPrevBox = false;

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

  // 宠物：当前睡觉/工作中/等待帧（全屏绘制后同步脏矩形基准框）
  const int16_t cx = (int16_t)(UI_W / 2 + ox);
  const int16_t bottomY = (int16_t)(418 + oy);
  if (!nvaDraw(c, cx, bottomY, false, 1.0f)) {
    ui::aaFillCircle(c, cx, (int16_t)(400 + oy), 34, rgb565(20, 32, 52));
  }
  const int16_t hw = nvaFrameWidth() / 2 + 2;
  const int16_t fh = nvaFrameHeight();
  s_aodPrevX0 = (int16_t)(cx - hw);
  s_aodPrevX1 = (int16_t)(cx + hw);
  s_aodPrevY0 = (int16_t)(bottomY - fh - 2);
  s_aodPrevY1 = (int16_t)(bottomY + 2);
  if (s_aodPrevY0 < (int16_t)(210 + oy)) s_aodPrevY0 = (int16_t)(210 + oy);
  if (s_aodPrevY1 > (int16_t)(422 + oy)) s_aodPrevY1 = (int16_t)(422 + oy);
  s_aodHasPrevBox = true;

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

void pageHomeDrawAodPet(NovaCanvas& c, const NovaState& st, uint32_t nowMs,
                        int16_t* outDx, int16_t* outDy, int16_t* outDw, int16_t* outDh) {
  const int16_t ox = (int16_t)(ui::burnOffsetX(st.burnInStep) + aod::driftX(nowMs));
  const int16_t oy = (int16_t)(ui::burnOffsetY(st.burnInStep) + aod::driftY(nowMs));
  const int16_t cx = (int16_t)(UI_W / 2 + ox);
  const int16_t bottomY = (int16_t)(418 + oy);

  const int16_t hw = nvaFrameWidth() / 2 + 2;
  const int16_t fh = nvaFrameHeight();
  const int16_t minY = (int16_t)(210 + oy);
  const int16_t maxY = (int16_t)(422 + oy);
  int16_t curX0 = (int16_t)(cx - hw);
  int16_t curX1 = (int16_t)(cx + hw);
  int16_t curY0 = (int16_t)(bottomY - fh - 2);
  int16_t curY1 = (int16_t)(bottomY + 2);
  if (curX0 < 0) curX0 = 0;
  if (curX1 > UI_W) curX1 = UI_W;
  if (curY0 < minY) curY0 = minY;
  if (curY1 > maxY) curY1 = maxY;

  int16_t uX0 = s_aodHasPrevBox ? (curX0 < s_aodPrevX0 ? curX0 : s_aodPrevX0) : curX0;
  int16_t uY0 = s_aodHasPrevBox ? (curY0 < s_aodPrevY0 ? curY0 : s_aodPrevY0) : curY0;
  int16_t uX1 = s_aodHasPrevBox ? (curX1 > s_aodPrevX1 ? curX1 : s_aodPrevX1) : curX1;
  int16_t uY1 = s_aodHasPrevBox ? (curY1 > s_aodPrevY1 ? curY1 : s_aodPrevY1) : curY1;
  if (uX0 < 0) uX0 = 0;
  if (uX1 > UI_W) uX1 = UI_W;
  if (uY0 < minY) uY0 = minY;
  if (uY1 > maxY) uY1 = maxY;

  s_aodPrevX0 = curX0; s_aodPrevY0 = curY0; s_aodPrevX1 = curX1; s_aodPrevY1 = curY1;
  s_aodHasPrevBox = true;

  c.fillRect(uX0, uY0, (int16_t)(uX1 - uX0), (int16_t)(uY1 - uY0), ui::COL_BG);
  if (!nvaDraw(c, cx, bottomY, false, 1.0f)) {
    ui::aaFillCircle(c, cx, (int16_t)(400 + oy), 34, rgb565(20, 32, 52));
  }

  if (outDx) *outDx = uX0;
  if (outDy) *outDy = uY0;
  if (outDw) *outDw = (int16_t)(uX1 - uX0);
  if (outDh) *outDh = (int16_t)(uY1 - uY0);
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
  const ui::Rect dockHit = ui::isTideTheme()
      ? ui::Rect{ 0, TIDE_DOCK_TOP, UI_W, (int16_t)(UI_H - TIDE_DOCK_TOP) }
      : dockRect(st);
  if (st.turnError && dockHit.contains(x, y)) {
    return UiAction{ UiAction::DOCK_TOGGLE, 0 };
  }

  // Demo 动作预览仅通过串口 {"cmd":"demo","anim":N} 调试使用，避免日常误触进入。
  const int16_t stageTop = ui::isTideTheme() ? TIDE_STRIP_H : STAGE_TOP;
  const int16_t stageBot = ui::isTideTheme() ? TIDE_STAGE_BOTTOM : STAGE_BOTTOM;
  if (y >= stageTop && y <= stageBot) {
    const int16_t ox = ui::burnOffsetX(st.burnInStep);
    const int16_t oy = ui::burnOffsetY(st.burnInStep);
    int16_t curX = (int16_t)(s_petX + 0.5f) + ox;
    int16_t dx = x - curX;
    if (dx < 0) dx = -dx;
    const int16_t petHitTop = ui::isTideTheme() ? (int16_t)(136 + oy) : (int16_t)(104 + oy);
    if (dx <= 68 && y >= petHitTop) {
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
  return false;
}

void pageHomeOnEnter(uint32_t nowMs) {
  (void)nowMs;
  s_hasPrevBox = false;
  s_prevHadBubble = false;
}

void pageHomeOnExit() {
  s_hasPrevBox = false;
  s_prevHadBubble = false;
  s_squashActive = false;
  s_pendingSquashTap = false;
  s_sparkFrames = 0;
  s_bubbleBuf[0] = '\0';
  s_bubbleUntilMs = 0;
  s_pendingBubbleMs = 0;
}

void pageHomeOnThemeChange() {
  s_hasPrevBox = false;
  s_prevHadBubble = false;
  s_tideBgValid = false;
  s_demoAnim = -1;
  s_squashActive = false;
  s_pendingSquashTap = false;
  s_sparkFrames = 0;
  s_bubbleBuf[0] = '\0';
  s_bubbleUntilMs = 0;
  s_pendingBubbleMs = 0;
}
