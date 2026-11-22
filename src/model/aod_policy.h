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

inline uint32_t idleMsFor(uint16_t idleSec) {
  uint32_t s = (idleSec < kMinIdleSec) ? kMinIdleSec : idleSec;
  return s * 1000u;
}

// 进入：模式允许 + 供电条件满足 + 没有活 + 屏亮着 + 没翻扣 + 闲够久。
// 阈值用毫秒传进来：电池常开时调用方会把它压到 batSleepSec，免得 30s 先熄屏。
inline bool shouldEnter(uint8_t mode, bool usb, uint16_t run, uint16_t wait, bool approval,
                        bool apMode, bool screenOff, bool dimmed, uint32_t idleMs,
                        uint32_t thresholdMs) {
  if (mode != USB && mode != ALWAYS) return false;
  if (mode == USB && !usb) return false;
  if (approval || apMode || screenOff || dimmed) return false;
  if (run > 0 || wait > 0) return false;
  if (thresholdMs < (uint32_t)kMinIdleSec * 1000u) thresholdMs = (uint32_t)kMinIdleSec * 1000u;
  return idleMs >= thresholdMs;
}

// 退出：比进入宽。翻扣（dimmed）不算退出 —— 那是主动遮屏，保持 AOD 只把亮度压到 0。
inline bool shouldExit(uint8_t mode, bool usb, uint16_t run, uint16_t wait, bool approval,
                       bool apMode, bool screenOff, uint8_t pct) {
  if (mode != USB && mode != ALWAYS) return true;
  if (mode == USB && !usb) return true;
  if (approval || apMode || screenOff) return true;
  if (run > 0 || wait > 0) return true;
  if (!usb && pct <= kLowBattPct) return true;
  return false;
}

inline bool isNight(uint8_t hour) { return hour >= 23 || hour < 7; }

// 夜间亮度档：0=封顶 1 档，1=封顶 2 档，2=跟随用户；23-07 点一律不超过 1 档。
inline uint8_t brightness(uint8_t userBright, uint8_t nightCap, bool night) {
  uint8_t cap = (nightCap == 0) ? 1 : ((nightCap == 1) ? 2 : userBright);
  if (night && cap > 1) cap = 1;
  return (userBright < cap) ? userBright : cap;
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

}  // namespace aod
