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
  CHECK(brightness(4, 0, false) == 1);
  CHECK(brightness(4, 0, true) == 1);
  CHECK(brightness(4, 1, false) == 2);
  CHECK(brightness(4, 1, true) == 1);
  CHECK(brightness(4, 2, false) == 4);
  CHECK(brightness(4, 2, true) == 1);
  CHECK(brightness(1, 0, false) == 1);
  CHECK(brightness(0, 1, false) == 0);

  // ── 夜间判定 ────────────────────────────────────────────────────
  CHECK(!isNight(22));
  CHECK(isNight(23));
  CHECK(isNight(0));
  CHECK(isNight(6));
  CHECK(!isNight(7));

  // ── 慢漂移：有界、10 分钟回归、每 2.5 分钟换一档 ────────────────
  for (uint32_t t = 0; t < 1800000u; t += 7919u) {
    CHECK(driftX(t) <= 3 && driftX(t) >= -3);
    CHECK(driftY(t) <= 2 && driftY(t) >= -2);
  }
  CHECK(driftX(0) == driftX(600000u));
  CHECK(driftY(0) == driftY(600000u));
  CHECK(driftX(0) != driftX(150000u));
  CHECK(driftX(150000u) == driftX(150000u + 600000u));

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
