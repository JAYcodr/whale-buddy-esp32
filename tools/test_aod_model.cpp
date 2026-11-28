// Host-side tests for src/model/aod_policy.h（AOD 纯策略层）。
// Build/run: bash tools/test_aod.sh
//
// 这里锁的是"什么时候进 / 什么时候退 / 多亮 / 漂到哪"，
// 跟字体、画布、I2C 全都无关，所以可以脱离板子跑。

#include "model/aod_policy.h"

#include <cstdio>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
  g_checks++; \
  if (!(cond)) { \
    g_fail++; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
  } \
} while (0)

int main() {
  using namespace aod;

  // ── 进入矩阵 ─────────────────────────────────────────────────────
  // 参数：mode usb approval apMode screenOff dimmed idleMs thresholdMs
  CHECK(!shouldEnter(OFF, true, false, false, false, false, 999999, 120000));
  CHECK(!shouldEnter(USB, false, false, false, false, false, 999999, 120000));
  CHECK(!shouldEnter(USB, true, false, false, false, false, 119999, 120000));
  CHECK(shouldEnter(USB, true, false, false, false, false, 120000, 120000));
  CHECK(shouldEnter(ALWAYS, false, false, false, false, false, 120000, 120000));
  CHECK(!shouldEnter(ALWAYS, false, true, false, false, false, 999999, 120000));
  CHECK(!shouldEnter(ALWAYS, false, false, true, false, false, 999999, 120000));
  CHECK(!shouldEnter(ALWAYS, false, false, false, true, false, 999999, 120000));
  CHECK(!shouldEnter(ALWAYS, false, false, false, false, true, 999999, 120000));
  // 阈值下限：写坏成 5s 也按 30s 算
  CHECK(!shouldEnter(USB, true, false, false, false, false, 29999, 5000));
  CHECK(shouldEnter(USB, true, false, false, false, false, 30000, 5000));
  CHECK(idleMsFor(0) == 30000u);
  CHECK(idleMsFor(120) == 120000u);

  // 闲置只看端侧输入
  CHECK(idleSince(100000, 40000) == 60000u);
  CHECK(idleSince(100000, 100000) == 0u);
  CHECK(idleSince(5000, 90000) == 0u);   // 回绕/异常不返回负数

  // ── 退出矩阵 ─────────────────────────────────────────────────────
  // 参数：mode usb approval apMode screenOff pct
  CHECK(shouldExit(OFF, true, false, false, false, 80));
  CHECK(shouldExit(USB, false, false, false, false, 80));      // 拔电
  CHECK(shouldExit(USB, true, true, false, false, 80));        // 审批
  CHECK(shouldExit(USB, true, false, true, false, 80));        // AP
  CHECK(shouldExit(USB, true, false, false, true, 80));        // 熄屏
  CHECK(!shouldExit(USB, true, false, false, false, 80));      // 待机保持
  CHECK(!shouldExit(ALWAYS, false, false, false, false, 50));  // 电池常开
  CHECK(shouldExit(ALWAYS, false, false, false, false, 10));   // 低电自救
  CHECK(shouldExit(ALWAYS, false, false, false, false, 9));
  CHECK(!shouldExit(USB, true, false, false, false, 5));       // 插电低电不算退出

  // ── 亮度 ────────────────────────────────────────────────────────
  // 用户 80%：封顶 1 档 39%、封顶 2 档 59%、跟随 = 80%；夜间一律 ≤39%。
  CHECK(brightnessPct(80, 0, false) == kCap1Pct);
  CHECK(brightnessPct(80, 0, true) == kCap1Pct);
  CHECK(brightnessPct(80, 1, false) == kCap2Pct);
  CHECK(brightnessPct(80, 1, true) == kCap1Pct);
  CHECK(brightnessPct(80, 2, false) == 80);
  CHECK(brightnessPct(80, 2, true) == kCap1Pct);
  // 用户本来就比封顶暗 —— 不动。
  CHECK(brightnessPct(30, 0, false) == 30);
  CHECK(brightnessPct(20, 1, false) == 20);
  CHECK(brightnessPct(100, 2, false) == 100);

  // ── 夜间判定 ────────────────────────────────────────────────────
  CHECK(!isNight(22));
  CHECK(isNight(23));
  CHECK(isNight(0));
  CHECK(isNight(6));
  CHECK(!isNight(7));

  // ── 语音静音时段（23–8，和 AOD 的 23–7 刻意不同）──────────────────
  CHECK(!isQuietHour(22));
  CHECK(isQuietHour(23));
  CHECK(isQuietHour(7));
  CHECK(!isQuietHour(8));

  // ── 慢漂移：有界、10 分钟回归、每 2.5 分钟换一档 ────────────────
  for (uint32_t t = 0; t < 1800000u; t += 7919u) {
    CHECK(driftX(t) <= 3 && driftX(t) >= -3);
    CHECK(driftY(t) <= 2 && driftY(t) >= -2);
  }
  CHECK(driftX(0) == driftX(600000u));
  CHECK(driftY(0) == driftY(600000u));
  CHECK(driftX(0) != driftX(150000u));
  CHECK(driftX(150000u) == driftX(150000u + 600000u));

  // ── 优雅退场状态机（播完当前轮 + 书签 + 哈欠 → AOD）───────────────
  {
    // 1. 正在播 HUM（非平静动作）：到点不抢断，等 loopCycles 递增才退入 YAWN 并推进书签
    RetireMachine rm;
    RetireInput in{};
    in.nowMs = 120000;
    in.canRetire = true;
    in.retireToAod = true;
    in.onHomePage = true;
    in.nvaReady = true;
    in.animKey = 20;  // HUM
    in.loopCycles = 5;

    auto r = rm.step(in);
    CHECK(r.phase == RetirePhase::DRAINING && !r.enterAodNow && !r.enterYawnNow && !r.bookmarkAdvance);

    in.nowMs = 124000;
    r = rm.step(in);  // 同一轮未结束
    CHECK(r.phase == RetirePhase::DRAINING && !r.enterYawnNow);

    in.nowMs = 126000;
    in.loopCycles = 6;  // 本轮播完
    r = rm.step(in);
    CHECK(r.phase == RetirePhase::YAWN && r.enterYawnNow && r.bookmarkAdvance && !r.enterAodNow);

    // 2. YAWN 阶段：先看到 YAWN 帧再等 oneShotEnded
    in.nowMs = 126200;
    in.isYawnPose = true;
    in.oneShotEnded = false;
    r = rm.step(in);
    CHECK(r.phase == RetirePhase::YAWN && !r.enterAodNow);

    in.nowMs = 136000;
    in.oneShotEnded = true;
    r = rm.step(in);
    CHECK(r.phase == RetirePhase::SLEEPING && r.enterAodNow);
  }

  {
    // 2. 平静 IDLE 到点：跳过 DRAINING 直接打哈欠，且不推进书签
    RetireMachine rm;
    RetireInput in{};
    in.nowMs = 120000;
    in.canRetire = true;
    in.retireToAod = true;
    in.onHomePage = true;
    in.nvaReady = true;
    in.isCalmPose = true;
    auto r = rm.step(in);
    CHECK(r.phase == RetirePhase::YAWN && r.enterYawnNow && !r.bookmarkAdvance);

    // 中途用户交互 -> 立即归位 ACTIVE
    in.canRetire = false;
    in.nowMs = 122000;
    r = rm.step(in);
    CHECK(r.phase == RetirePhase::ACTIVE && !r.enterAodNow);
  }

  {
    // 3. 长任务跑着（busyTask）：等当前循环播完后跳过哈欠直接进 AOD
    RetireMachine rm;
    RetireInput in{};
    in.nowMs = 120000;
    in.canRetire = true;
    in.retireToAod = true;
    in.onHomePage = true;
    in.nvaReady = true;
    in.busyTask = true;
    in.animKey = 15;  // CODING
    in.loopCycles = 2;
    auto r = rm.step(in);
    CHECK(r.phase == RetirePhase::DRAINING);

    in.nowMs = 140000;  // 触发硬上限 kMaxDrainMs=15s
    r = rm.step(in);
    CHECK(r.phase == RetirePhase::SLEEPING && r.enterAodNow && !r.enterYawnNow);
  }

  {
    // 4. 非主屏 / 无动画资产时直接进 AOD，不拖延
    RetireMachine rm;
    RetireInput in{};
    in.nowMs = 120000;
    in.canRetire = true;
    in.retireToAod = true;
    in.onHomePage = false;
    in.nvaReady = true;
    auto r = rm.step(in);
    CHECK(r.phase == RetirePhase::SLEEPING && r.enterAodNow);
  }

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
