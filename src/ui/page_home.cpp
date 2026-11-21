#include "ui/pages.h"
#include "anim/nva.h"
#include "ui/theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr ui::Rect BTN_CANCEL_PORTAL{ 94, 304, 180, 46 };
constexpr int16_t STAGE_TOP    = 62;
constexpr int16_t STAGE_BOTTOM = 304;
constexpr int16_t PET_MIN_X    = 80;
constexpr int16_t PET_MAX_X    = 288;
constexpr int16_t PET_BASE_Y   = 296;
constexpr ui::Rect PILL_RECT{ 96, 308, 176, 28 };

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

char     s_bubbleBuf[48]    = "";
uint32_t s_bubbleUntilMs    = 0;
uint32_t s_pendingBubbleMs  = 0;

int16_t s_prevX0 = 94, s_prevY0 = 110, s_prevX1 = 274, s_prevY1 = 304;
bool    s_hasPrevBox = false;

const char* const kPetQuotes[] = {
  "在呢主人!",
  "摸摸头~",
  "今天也要加油!",
  "收到指令啦",
};
uint8_t s_quoteIdx = 0;

void setBubble(const char* txt, uint32_t durMs, uint32_t nowMs = 0) {
  if (!txt || !txt[0]) return;
  strncpy(s_bubbleBuf, txt, sizeof(s_bubbleBuf) - 1);
  s_bubbleBuf[sizeof(s_bubbleBuf) - 1] = '\0';
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
  return k == NvaAnimKey::IDLE || k == NvaAnimKey::CLICK ||
         k == NvaAnimKey::SUCCESS || k == NvaAnimKey::SLEEP;
}

void drawSpark(NovaCanvas& c, int16_t x, int16_t y, uint16_t col, int16_t size) {
  c.drawLine(x - size, y, x + size, y, col);
  c.drawLine(x, y - size, x, y + size, col);
}

void updateRoam(const NovaState& st, uint32_t frame) {
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

  float diff = s_targetX - s_petX;
  float adiff = diff < 0 ? -diff : diff;
  if (adiff > 6.0f) s_facingRight = (diff > 0.0f);

  if (adiff > 1.2f) {
    if (s_walkLeadUntilMs == 0 || (int32_t)(st.nowMs - s_walkLeadUntilMs) >= 0) {
      float maxSpeedPxPerSec = s_runSpeed ? 92.0f : 38.0f;
      if (adiff < 16.0f) {
        // Gentle ease-out deceleration on approach (inspired by dsh-pet walk tail settle).
        float factor = adiff / 16.0f;
        if (factor < 0.35f) factor = 0.35f;
        maxSpeedPxPerSec *= factor;
      }
      float step = maxSpeedPxPerSec * ((float)dtMs / 1000.0f);
      if (adiff <= step) {
        s_petX = s_targetX;
        s_runSpeed = false;
        s_userTargetLock = false;
        pageHomeTriggerSquash(st.nowMs);
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

  // Execute LLM choreography script when in autonomous idle/patrol mode.
  // Never advance to the next step mid-animation: require arrival + min duration + >=1 full clip cycle.
  if (st.nChoreoSteps > 0 && s_demoAnim < 0 && st.run == 0 && st.wait == 0 &&
      !st.approvalActive && !s_userTargetLock) {
    if (!s_choreoStepInit) {
      if (s_choreoIdx >= st.nChoreoSteps) s_choreoIdx = 0;
      const ChoreoStep& step = st.choreoSteps[s_choreoIdx];
      s_targetX = (float)step.x;
      float walkDist = fabsf(s_targetX - s_petX);
      s_runSpeed = step.run;
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

  // Built-in local fallback patrol when no LLM script is active (millisecond-based, not frame-based).
  if (s_petX == s_targetX && (int32_t)(st.nowMs - s_nextRoamMs) >= 0 && st.wait == 0 && !st.approvalActive) {
    if (s_demoAnim == (int)NvaAnimKey::SLEEP) {
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

void drawStageContent(NovaCanvas& c, const NovaState& st, uint32_t frame,
                      int16_t* outDx, int16_t* outDy, int16_t* outDw, int16_t* outDh) {
  updateRoam(st, frame);

  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const int16_t cx = (int16_t)(s_petX + 0.5f) + ox;

  float diff = s_targetX - s_petX;
  bool walking = (diff < -1.2f || diff > 1.2f);
  int16_t hop = walking ? (int16_t)(fabsf(sinf(frame * 0.28f)) * 5.0f) : 0;
  const int16_t bottomY = PET_BASE_Y + oy - hop;

  int16_t curX0 = cx - 84;
  int16_t curX1 = cx + 84;
  int16_t curY0 = bottomY - 190;
  int16_t curY1 = PET_BASE_Y + 6;

  bool showBubble = (s_bubbleBuf[0] && s_bubbleUntilMs != 0 && (int32_t)(s_bubbleUntilMs - st.nowMs) > 0);
  if (s_bubbleBuf[0] && !showBubble && s_bubbleUntilMs != 0) {
    s_bubbleBuf[0] = '\0';
    s_bubbleUntilMs = 0;
  }

  if (showBubble) {
    curX0 = (cx - 104 < curX0) ? (cx - 104) : curX0;
    curX1 = (cx + 104 > curX1) ? (cx + 104) : curX1;
    curY0 = STAGE_TOP;
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
    int16_t px = cx + (int16_t)(cosf(a) * 62.0f);
    int16_t py = (bottomY - 82) + (int16_t)(sinf(a * 1.15f) * 34.0f);
    if (px >= 4 && px < UI_W - 4 && py >= STAGE_TOP + 4 && py < STAGE_BOTTOM - 4) {
      ui::aaFillCircle(c, px, py, (i & 1) ? 2 : 3, (i & 1) ? ui::COL_MINT : ui::COL_ACCENT);
    }
  }

  if (s_sparkFrames > 0) {
    float prog = 1.0f - (float)s_sparkFrames / 24.0f;
    for (int i = 0; i < 6; i++) {
      float a = prog * 0.7f + i * 1.0472f;
      int d = 36 + (int)(prog * 36.0f);
      int16_t sx = cx + (int16_t)(cosf(a) * d);
      int16_t sy = (bottomY - 82) + (int16_t)(sinf(a) * d);
      if (sx > 6 && sx < UI_W - 6 && sy > STAGE_TOP + 6 && sy < STAGE_BOTTOM - 6) {
        drawSpark(c, sx, sy, (i & 1) ? ui::COL_MINT : ui::COL_GOLD, (s_sparkFrames > 12) ? 3 : 2);
      }
    }
    s_sparkFrames--;
  }

  bool flipX = s_facingRight && spriteAllowsMirror(nvaActiveState());
  float squashY = calcSquashScaleY(st.nowMs);
  if (!nvaDraw(c, cx, bottomY, flipX, squashY)) {
    ui::aaFillCircle(c, cx, bottomY - 70, 34, auraCol);
    ui::aaFillCircle(c, cx - 10, bottomY - 80, 9, rgb565(224, 246, 255));
  }

  if (showBubble) {
    ui::TextMetrics bm = ui::measureText(c, ui::fontCjk14(), s_bubbleBuf);
    int16_t bw = (int16_t)bm.w + 24;
    if (bw > UI_W - 24) bw = UI_W - 24;
    int16_t bx = cx - bw / 2;
    if (bx < 12) bx = 12;
    if (bx + bw > UI_W - 12) bx = UI_W - 12 - bw;
    int16_t by = STAGE_TOP + 6;
    ui::aaFillRoundRect(c, bx, by, bw, 28, 14, ui::COL_CARD_HI, ui::COL_ACCENT);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), { bx, by, bw, 28 }, s_bubbleBuf, ui::COL_TEXT, ui::COL_CARD_HI);
  }

  if (outDx) *outDx = uX0;
  if (outDy) *outDy = uY0;
  if (outDw) *outDw = uX1 - uX0;
  if (outDh) *outDh = uY1 - uY0;
}

void drawStatusPill(NovaCanvas& c, const NovaState& st) {
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);

  uint16_t dotCol = ui::COL_ACCENT;
  const char* stateTag = "待机陪伴中";
  if (s_demoAnim >= 0) {
    static constexpr const char* kDemoTags[9] = {
      "展示: 待机", "展示: 思考", "展示: 编码",
      "展示: 等待", "展示: 庆祝", "展示: 委屈",
      "展示: 互动", "展示: 打盹", "展示: 舞动",
    };
    dotCol = ui::COL_VIOLET;
    stateTag = kDemoTags[s_demoAnim % 9];
  } else if (st.wait > 0) {
    dotCol = ui::COL_AMBER;
    stateTag = "等待确认";
  } else if (st.run > 0) {
    dotCol = ui::COL_MINT;
    stateTag = "思考执行中";
  } else if (st.nChoreoSteps > 0 && st.choreoMood[0]) {
    dotCol = ui::COL_ACCENT;
    stateTag = st.choreoMood;
  } else if (!st.wifiConnected || !st.mqttConnected) {
    dotCol = ui::COL_ACCENT;
    stateTag = "本地陪伴中";
  }

  ui::TextMetrics tm = ui::measureText(c, ui::fontCjk14(), stateTag);
  int16_t pw = (int16_t)tm.w + 34;
  if (pw > 280) pw = 280;
  int16_t px = (int16_t)((UI_W - pw) / 2 + ox);
  int16_t py = (int16_t)(308 + oy);
  ui::drawCard(c, px, py, pw, 28, ui::COL_CARD, ui::COL_BORDER, 14);
  ui::aaFillCircle(c, px + 12, py + 14, 3, dotCol);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), { (int16_t)(px + 18), py, (int16_t)(pw - 22), 28 },
                             stateTag, ui::COL_DIM, ui::COL_CARD);
}

}  // namespace

int pageHomeDemoOverride() {
  return s_demoAnim;
}

int pageHomeChoreoAnim() {
  return s_choreoAnim;
}

bool pageHomeIsWalking() {
  float diff = s_targetX - s_petX;
  return (diff < -1.2f || diff > 1.2f);
}

void pageHomeTriggerSquash(uint32_t nowMs) {
  s_squashStartMs = nowMs;
  s_squashActive = true;
}

void pageHomeDrawStage(NovaCanvas& c, const NovaState& st, uint32_t frame,
                       int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH) {
  drawStageContent(c, st, frame, outX, outY, outW, outH);
}

void pageHomeDraw(NovaCanvas& c, const NovaState& st, uint32_t frame) {
  c.fillScreen(ui::COL_BG);
  ui::drawTopBar(c, st, 0);

  if (st.wifiApMode && !st.wifiConnected) {
    ui::drawCard(c, 24, 80, 320, 290, ui::COL_CARD, ui::COL_AMBER, 20);
    ui::drawText(c, ui::fontCjk16(), 44, 118, "首次配网 / AP 模式", ui::COL_AMBER, ui::COL_CARD);

    char step1[64];
    snprintf(step1, sizeof(step1), "1. 手机连接热点: %s", st.apSsid[0] ? st.apSsid : "Nova-XXXX");
    ui::drawText(c, ui::fontCjk14(), 44, 158, step1, ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), 44, 188, "2. 浏览器访问: http://192.168.4.1", ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), 44, 218, "3. 填写 WiFi 密码与 MQTT Broker", ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), 44, 256, "默认 Broker: 192.168.31.15:1883", ui::COL_DIM, ui::COL_CARD);

    ui::drawButton(c, BTN_CANCEL_PORTAL, "暂不配网", false, ui::COL_AMBER);
    return;
  }

  s_hasPrevBox = false;
  drawStageContent(c, st, frame, nullptr, nullptr, nullptr, nullptr);
  drawStatusPill(c, st);

  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);
  const int16_t dockX = (int16_t)(20 + ox);
  const int16_t dockY = (int16_t)(348 + oy);
  const int16_t dockW = 328;
  const int16_t dockH = 80;
  ui::drawCard(c, dockX, dockY, dockW, dockH, ui::COL_CARD, ui::COL_BORDER, 18);

  const bool hostLinked = (st.wifiConnected && st.mqttConnected && st.lastMqttRxMs > 0);
  if (hostLinked) {
    char statsLine[64];
    snprintf(statsLine, sizeof(statsLine), "运行 %u · 等待 %u · 会话 %u", st.run, st.wait, st.sess);
    ui::drawText(c, ui::fontCjk14(), dockX + 16, dockY + 30, statsLine, ui::COL_TEXT, ui::COL_CARD);

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

    char metaLine[96];
    if (st.activeModel[0]) {
      char raw[80];
      snprintf(raw, sizeof(raw), "当前模型 · %s", st.activeModel);
      ui::fitUtf8Line(c, ui::fontCjk14(), raw, dockW - 32, metaLine, sizeof(metaLine));
    } else {
      snprintf(metaLine, sizeof(metaLine), "轻触舞台逗诺瓦跑动 · 左右划切页");
    }
    ui::drawText(c, ui::fontCjk14(), dockX + 16, dockY + 62, metaLine, ui::COL_DIM, ui::COL_CARD);
  } else {
    ui::drawText(c, ui::fontCjk14(), dockX + 16, dockY + 30, "离线陪伴模式 · 本地交互就绪", ui::COL_TEXT, ui::COL_CARD);
    ui::drawText(c, ui::fontCjk14(), dockX + 16, dockY + 62, "点空白处让诺瓦跑过去 · 点胶囊换动作", ui::COL_DIM, ui::COL_CARD);
  }
}

UiAction pageHomeTap(int16_t x, int16_t y, const NovaState& st) {
  if (st.wifiApMode && !st.wifiConnected && BTN_CANCEL_PORTAL.contains(x, y)) {
    return UiAction{ UiAction::CANCEL_PORTAL, 0 };
  }

  if (PILL_RECT.contains(x, y)) {
    if (s_demoAnim < 0) s_demoAnim = 1;
    else if (s_demoAnim >= 8) s_demoAnim = -1;
    else s_demoAnim++;
    s_pendingSquashTap = true;
    setBubble((s_demoAnim < 0) ? "恢复自动状态" : "切换动作!", 3500u, st.nowMs);
    return UiAction{ UiAction::PET_TAP, 2 };
  }

  if (y >= STAGE_TOP && y <= STAGE_BOTTOM) {
    int16_t curX = (int16_t)(s_petX + 0.5f);
    int16_t dx = x - curX;
    if (dx < 0) dx = -dx;
    if (dx <= 62 && y >= 112) {
      s_sparkFrames = 24;
      s_pendingSquashTap = true;
      setBubble(kPetQuotes[s_quoteIdx % 4], 6500u, st.nowMs);
      s_quoteIdx++;
      return UiAction{ UiAction::PET_TAP, 0 };
    }
    int16_t tx = x;
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
