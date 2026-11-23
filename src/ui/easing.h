#pragma once
// 统一缓动工具：所有动效都应从这里取曲线，避免各页面各写一份 1-(1-u)^3。
// 约定：输入 u 为归一化进度（任意值，内部钳到 [0,1]），输出为归一化位移。
// 全部 header-only + inline，主机模拟器（NOVA_HOST_SIM）同样可用。
#include <math.h>
#include <stdint.h>

namespace ease {

inline float clamp01(float u) { return u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u); }

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// 由起始时间与时长算进度；durMs 为 0 时直接视为完成。
inline float progress(uint32_t nowMs, uint32_t startMs, uint32_t durMs) {
  if (durMs == 0) return 1.0f;
  return clamp01((float)(uint32_t)(nowMs - startMs) / (float)durMs);
}

inline float outCubic(float u) {
  float v = 1.0f - clamp01(u);
  return 1.0f - v * v * v;
}

// 比 cubic 收尾更长更柔：用于“落定感”要强的进入动画（弹窗、浮层）。
inline float outQuart(float u) {
  float v = 1.0f - clamp01(u);
  return 1.0f - v * v * v * v;
}

inline float outQuint(float u) {
  float v = 1.0f - clamp01(u);
  return 1.0f - v * v * v * v * v;
}

// 离场 / 往返类动画：两端都柔。
inline float inOutCubic(float u) {
  u = clamp01(u);
  if (u < 0.5f) return 4.0f * u * u * u;
  float v = -2.0f * u + 2.0f;
  return 1.0f - v * v * v * 0.5f;
}

// 带过冲回弹；c1=1.70158 时峰值约 1.10（与主屏挤压回弹一致）。
inline float outBack(float u, float c1 = 1.70158f) {
  u = clamp01(u);
  const float c3 = c1 + 1.0f;
  float v = u - 1.0f;
  return 1.0f + c3 * v * v * v + c1 * v * v;
}

// 帧率无关的指数趋近：每帧 current += (target-current)*approachFactor(dtMs, tauMs)。
// tauMs 为时间常数（约 63% 路程所需时间），用于跟手值平滑、颜色/位置补间。
inline float approachFactor(uint32_t dtMs, float tauMs) {
  if (tauMs <= 0.0f) return 1.0f;
  // 每帧只调用个位数次，直接用 expf 即可，不值得做近似。
  return 1.0f - expf(-(float)dtMs / tauMs);
}

}  // namespace ease
