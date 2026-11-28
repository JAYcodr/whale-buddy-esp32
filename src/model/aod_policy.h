#pragma once
#include <stdint.h>

// AOD（熄屏显示）纯策略层：不依赖 Arduino / I2C / NVS，主机单测直接喂参数。
// 规则写在这里，main.cpp 只做接线，UI 只做画图 —— 免得三处各判一次。
namespace aod {

enum Mode : uint8_t {
  OFF = 0,      // 关
  USB = 1,      // 仅插电（默认）
  ALWAYS = 2,   // 插电 + 电池常开
};

constexpr uint16_t kMinIdleSec  = 30;   // 进入待机的下限，防止设置写坏后秒进
constexpr uint8_t  kLowBattPct  = 10;   // 电池常开时低于这个就退出并熄屏

// 闲置时长只看端侧输入：触摸 / 按键。**不看宿主的运行会话**——
// 一个跑五六个小时的任务不该让屏幕亮一整天，到点就该进表盘。
inline uint32_t idleSince(uint32_t nowMs, uint32_t lastInputMs) {
  return (nowMs > lastInputMs) ? (nowMs - lastInputMs) : 0;
}

inline uint32_t idleMsFor(uint16_t idleSec) {
  uint32_t s = (idleSec < kMinIdleSec) ? kMinIdleSec : idleSec;
  return s * 1000u;
}

// 进入：模式允许 + 供电条件满足 + 屏亮着 + 没翻扣 + 端侧没输入够久。
// 审批 / 配网是例外（要人看）。**任务是否在跑不参与判定**：长任务照样进表盘。
inline bool shouldEnter(uint8_t mode, bool usb, bool approval, bool apMode, bool screenOff,
                        bool dimmed, uint32_t idleMs, uint32_t thresholdMs) {
  if (mode != USB && mode != ALWAYS) return false;
  if (mode == USB && !usb) return false;
  if (approval || apMode || screenOff || dimmed) return false;
  if (thresholdMs < (uint32_t)kMinIdleSec * 1000u) thresholdMs = (uint32_t)kMinIdleSec * 1000u;
  return idleMs >= thresholdMs;
}

// 退出：比进入宽。翻扣（dimmed）不算退出 —— 那是主动遮屏，保持 AOD 只把亮度压到 0。
// 任务开始/结束也不退出：表盘上的状态行会自己更新，屏幕不用整夜亮着。
inline bool shouldExit(uint8_t mode, bool usb, bool approval, bool apMode, bool screenOff,
                       uint8_t pct) {
  if (mode != USB && mode != ALWAYS) return true;
  if (mode == USB && !usb) return true;
  if (approval || apMode || screenOff) return true;
  if (!usb && pct <= kLowBattPct) return true;
  return false;
}

inline bool isNight(uint8_t hour) { return hour >= 23 || hour < 7; }

// 语音静音用 23–8；AOD 亮度用 23–7（isNight）。两者是有意的差异，别合并。
inline bool isQuietHour(uint8_t h) { return h >= 23 || h < 8; }

// 亮度保护用的封顶线（百分比）。数值对应改百分比之前的 1 档 / 2 档：
// 老 BRIGHT_LUT = {50,100,150,200,255}，1 档 = 100/255 ≈ 39%，2 档 = 150/255 ≈ 59%。
constexpr uint8_t kCap1Pct = 39;
constexpr uint8_t kCap2Pct = 59;

// 亮度保护：0=封顶到 1 档，1=封顶到 2 档，2=跟随用户；23-07 点一律不超过 1 档。
// userPct 是 20..100 的用户亮度，返回真正要下发给背光的百分比。
inline uint8_t brightnessPct(uint8_t userPct, uint8_t nightCap, bool night) {
  uint8_t cap = (nightCap == 0) ? kCap1Pct : ((nightCap == 1) ? kCap2Pct : userPct);
  if (night && cap > kCap1Pct) cap = kCap1Pct;
  return (userPct < cap) ? userPct : cap;
}

// 慢漂移：10 分钟一轮、四个落点。配合现有 ±2px/30s 的 burnOffset，
// 让大号时间块不会长时间压在同一片像素上。
inline int16_t driftX(uint32_t nowMs) {
  static const int8_t DX[4] = { 0, 3, 0, -3 };
  return DX[(nowMs / 150000u) % 4u];
}

inline int16_t driftY(uint32_t nowMs) {
  static const int8_t DY[4] = { 0, -2, 0, 2 };
  return DY[(nowMs / 150000u) % 4u];
}

// ── 优雅退场状态机（等当前动画周期播完 + 书签推进 + 哈欠前奏 → 进 AOD/小憩）──────
// 代替「一到时间立刻截断当前动作/提前 45 秒硬睡死」：
//   ACTIVE → (到闲置阈值且有动作在播) DRAINING → YAWN → (播完一轮哈欠) 通知进 AOD
// 任何用户输入、审批、配网或离线低电会立即打断退场并归位 ACTIVE。
enum class RetirePhase : uint8_t {
  ACTIVE   = 0,
  DRAINING = 1,
  YAWN     = 2,
  SLEEPING = 3,
};

constexpr uint32_t kMaxDrainMs = 15000u;  // 等待当前循环播完的硬上限，防长任务/异常卡死 AOD
constexpr uint32_t kMaxYawnMs  = 10500u;  // yawn.nva 100 帧 @10fps = 10s，留 0.5s 裕量

struct RetireInput {
  uint32_t nowMs;
  bool     canRetire;       // 是否满足退场总开关（AOD 条件，或插电且 AOD=OFF 的长闲置小憩）
  bool     retireToAod;     // true=退场完进入 AOD；false=仅进入本地 SLEEP 姿态
  bool     onHomePage;      // 非主屏不演收尾动画，到点直接进 AOD
  bool     nvaReady;        // 无 .nva 资产时跳过等播完与哈欠
  bool     busyTask;        // st.run > 0 || st.wait > 0（长任务跳过哈欠，AOD 展示 WORKING）
  bool     walking;         // 主屏是否正在移动走路
  bool     isCalmPose;      // 当前是否已是 IDLE 或 SLEEP（无需等本轮播完）
  bool     isYawnPose;      // 当前前台渲染的动画是否为 YAWN
  bool     isSleepPose;     // 当前前台渲染的动画是否为 SLEEP
  bool     oneShotActive;   // 正在播放单次动作（click / success / error）
  bool     oneShotEnded;    // nvaIsEnded()
  uint8_t  animKey;         // 当前正在播的 NvaAnimKey
  uint32_t loopCycles;      // nvaLoopCycles()
};

struct RetireStepResult {
  RetirePhase phase           = RetirePhase::ACTIVE;
  bool        enterYawnNow    = false;  // 刚切入 YAWN，调用方需 restart 一次 YAWN 动画
  bool        enterAodNow     = false;  // 退场演出完成，立即调用 beginAodEnter()
  bool        bookmarkAdvance = false;  // 当前剧本/动作已完整播完，推进书签到下一项
};

struct RetireMachine {
  RetirePhase phase            = RetirePhase::ACTIVE;
  uint32_t    drainStartMs     = 0;
  uint32_t    drainStartCycles = 0;
  uint8_t     drainStartKey    = 0;
  bool        drainWasOneShot  = false;
  uint32_t    yawnStartMs      = 0;
  uint32_t    yawnStartCycles  = 0;
  bool        yawnSeenKey      = false;

  void reset() {
    phase = RetirePhase::ACTIVE;
    drainStartMs = 0;
    drainStartCycles = 0;
    drainStartKey = 0;
    drainWasOneShot = false;
    yawnStartMs = 0;
    yawnStartCycles = 0;
    yawnSeenKey = false;
  }

  RetireStepResult step(const RetireInput& in) {
    RetireStepResult out;
    if (!in.canRetire) {
      reset();
      return out;
    }

    if (phase == RetirePhase::ACTIVE) {
      if (!in.onHomePage || !in.nvaReady) {
        if (in.retireToAod) {
          phase = RetirePhase::SLEEPING;
          out.enterAodNow = true;
        }
        out.phase = phase;
        return out;
      }
      // 空闲呆站 / 已睡着且没在走路/单次反应 → 不必等周期，直接进入收尾
      if (in.isCalmPose && !in.walking && !in.oneShotActive) {
        if (in.busyTask || in.isSleepPose) {
          phase = RetirePhase::SLEEPING;
          out.enterAodNow = in.retireToAod;
        } else {
          phase = RetirePhase::YAWN;
          yawnStartMs = in.nowMs;
          yawnStartCycles = in.loopCycles;
          yawnSeenKey = false;
          out.enterYawnNow = true;
        }
        out.phase = phase;
        return out;
      }
      // 有动作/脚步正在演：保持该动作到本轮自然结束再收尾
      phase = RetirePhase::DRAINING;
      drainStartMs = in.nowMs;
      drainStartCycles = in.loopCycles;
      drainStartKey = in.animKey;
      drainWasOneShot = in.oneShotActive;
      out.phase = phase;
      return out;
    }

    if (phase == RetirePhase::DRAINING) {
      const bool timedOut = (uint32_t)(in.nowMs - drainStartMs) >= kMaxDrainMs;
      bool animDone = false;
      if (drainWasOneShot) {
        animDone = !in.oneShotActive || (in.animKey == drainStartKey && in.oneShotEnded);
      } else {
        animDone = (in.loopCycles > drainStartCycles) || in.isCalmPose;
      }
      if (!timedOut && (in.walking || !animDone)) {
        out.phase = phase;
        return out;
      }
      // 只有真正在播非平静动作/非单次点击时，播完才把日常书签推到下一幕
      out.bookmarkAdvance = !drainWasOneShot && !in.isCalmPose;
      if (in.busyTask) {
        phase = RetirePhase::SLEEPING;
        out.enterAodNow = in.retireToAod;
      } else {
        phase = RetirePhase::YAWN;
        yawnStartMs = in.nowMs;
        yawnStartCycles = in.loopCycles;
        yawnSeenKey = false;
        out.enterYawnNow = true;
      }
      out.phase = phase;
      return out;
    }

    if (phase == RetirePhase::YAWN) {
      // 长任务中途到来：不必继续哈欠，直接切入 AOD 显示 WORKING
      if (in.busyTask) {
        phase = RetirePhase::SLEEPING;
        out.enterAodNow = in.retireToAod;
        out.phase = phase;
        return out;
      }
      if (in.isYawnPose) yawnSeenKey = true;
      const bool timedOut = (uint32_t)(in.nowMs - yawnStartMs) >= kMaxYawnMs;
      const bool clipDone = yawnSeenKey && (in.oneShotEnded || in.loopCycles > yawnStartCycles);
      if (clipDone || timedOut) {
        phase = RetirePhase::SLEEPING;
        out.enterAodNow = in.retireToAod;
      }
      out.phase = phase;
      return out;
    }

    out.phase = phase;
    return out;
  }
};

}  // namespace aod
