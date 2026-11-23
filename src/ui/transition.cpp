#include "ui/transition.h"
#include "ui/canvas.h"
#include "ui/easing.h"
#include "ui/theme.h"

#include <stdlib.h>
#include <string.h>

#ifndef NOVA_HOST_SIM
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "hw/display.h"
#endif

namespace {

constexpr int16_t kSlideSplitY     = 46;  // Keep top clock/status steady during horizontal slides.
constexpr int16_t kSlidePillSplitY = 60;  // 启用圆点插值时同时固定圆点带（46..59），只重绘圆点小矩形。
constexpr int32_t kDimMinScale = 154; // round(0.6 * 256)
constexpr int32_t kDimSpan     = 256 - kDimMinScale;
// 模态弹窗位移量：小位移 + 淡变，比纯交叉淡化更有“落位”感，又不至于像整页滑动。
constexpr int kPopRisePx = 22;
constexpr int kPopSinkPx = 14;

uint16_t* s_transFrom = nullptr;
uint16_t* s_transTo   = nullptr;

#ifndef NOVA_HOST_SIM
inline uint16_t dimPixel(uint16_t c, uint32_t scale) {
  uint32_t r = (((c & 0xF800u) * scale) >> 8) & 0xF800u;
  uint32_t g = (((c & 0x07E0u) * scale) >> 8) & 0x07E0u;
  uint32_t b =  ((c & 0x001Fu) * scale) >> 8;
  return (uint16_t)(r | g | b);
}

void dimRows(uint16_t* dst, const uint16_t* src, int rows, int32_t scale) {
  if (rows <= 0) return;
  const size_t pxCount = (size_t)rows * UI_W;
  if (scale >= 256) {
    memcpy(dst, src, pxCount * sizeof(uint16_t));
    return;
  }
  if (scale <= 0) {
    memset(dst, 0, pxCount * sizeof(uint16_t));
    return;
  }

  const uint32_t sc = (uint32_t)scale;
  if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0) {
    uint32_t* dst32 = (uint32_t*)dst;
    const uint32_t* src32 = (const uint32_t*)src;
    const size_t pairs = pxCount >> 1;
    for (size_t i = 0; i < pairs; i++) {
      uint32_t p = src32[i];
      uint16_t d0 = dimPixel((uint16_t)(p & 0xFFFFu), sc);
      uint16_t d1 = dimPixel((uint16_t)(p >> 16), sc);
      dst32[i] = (uint32_t)d0 | ((uint32_t)d1 << 16);
    }
    if (pxCount & 1u) {
      dst[pxCount - 1] = dimPixel(src[pxCount - 1], sc);
    }
  } else {
    for (size_t i = 0; i < pxCount; i++) {
      dst[i] = dimPixel(src[i], sc);
    }
  }
}

inline uint16_t blendPixel(uint16_t cf, uint16_t ct, uint32_t invA, uint32_t alpha5) {
  uint32_t x = (cf | ((uint32_t)cf << 16)) & 0x07E0F81Fu;
  uint32_t y = (ct | ((uint32_t)ct << 16)) & 0x07E0F81Fu;
  uint32_t m = ((x * invA + y * alpha5) >> 5) & 0x07E0F81Fu;
  return (uint16_t)(m | (m >> 16));
}

void blendFrame(uint16_t* dst, const uint16_t* from, const uint16_t* to,
                size_t pxCount, uint32_t alpha5) {
  if (alpha5 == 0) {
    memcpy(dst, from, pxCount * sizeof(uint16_t));
    return;
  }
  if (alpha5 >= 32) {
    memcpy(dst, to, pxCount * sizeof(uint16_t));
    return;
  }

  const uint32_t invA = 32u - alpha5;
  if ((((uintptr_t)dst | (uintptr_t)from | (uintptr_t)to) & 3u) == 0) {
    uint32_t* dst32 = (uint32_t*)dst;
    const uint32_t* from32 = (const uint32_t*)from;
    const uint32_t* to32 = (const uint32_t*)to;
    const size_t pairs = pxCount >> 1;
    for (size_t i = 0; i < pairs; i++) {
      uint32_t f = from32[i];
      uint32_t t = to32[i];
      // Mask 0x07E0F81F isolates (G1, R0, B0); 0xF81F07E0 >> 5 isolates (R1, B1, G0).
      // 5-bit alpha (0..32) guarantees zero cross-channel carry inside 32-bit lanes.
      uint32_t loF = f & 0x07E0F81Fu;
      uint32_t loT = t & 0x07E0F81Fu;
      uint32_t lo  = ((loF * invA + loT * alpha5) >> 5) & 0x07E0F81Fu;

      uint32_t hiF = (f & 0xF81F07E0u) >> 5;
      uint32_t hiT = (t & 0xF81F07E0u) >> 5;
      uint32_t hi  = (hiF * invA + hiT * alpha5) & 0xF81F07E0u;

      dst32[i] = lo | hi;
    }
    if (pxCount & 1u) {
      dst[pxCount - 1] = blendPixel(from[pxCount - 1], to[pxCount - 1], invA, alpha5);
    }
  } else {
    for (size_t i = 0; i < pxCount; i++) {
      dst[i] = blendPixel(from[i], to[i], invA, alpha5);
    }
  }
}
#endif

}  // namespace

bool transBuffers(uint16_t** from, uint16_t** to) {
  if (!from || !to) return false;
  const size_t fbBytes = (size_t)UI_W * UI_H * sizeof(uint16_t);
#ifndef NOVA_HOST_SIM
  if (!s_transFrom) {
    s_transFrom = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!s_transTo) {
    s_transTo = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
#else
  if (!s_transFrom) s_transFrom = (uint16_t*)malloc(fbBytes);
  if (!s_transTo)   s_transTo   = (uint16_t*)malloc(fbBytes);
#endif
  if (!s_transFrom || !s_transTo) return false;
  *from = s_transFrom;
  *to   = s_transTo;
  return true;
}

void transRun(Arduino_Canvas& canvas, const uint16_t* from, const uint16_t* to,
              TransKind kind, uint32_t ms, const TransPillOpts* pills) {
  uint16_t* fb = canvas.getFramebuffer();
  const size_t pxCount = (size_t)UI_W * UI_H;
  const size_t fbBytes = pxCount * sizeof(uint16_t);

#ifdef NOVA_HOST_SIM
  (void)from;
  (void)kind;
  (void)ms;
  (void)pills;
  if (fb && to) memcpy(fb, to, fbBytes);
  return;
#else
  if (!fb || !from || !to) {
    if (fb && to) {
      memcpy(fb, to, fbBytes);
      hwDisplayPush();
    }
    return;
  }

  const bool usePillFollow = (pills && pills->fromDot >= 0 && pills->toDot >= 0 &&
                              pills->totalDots > 0 &&
                              (kind == TransKind::SLIDE_LEFT || kind == TransKind::SLIDE_RIGHT));
  const int16_t slideSplitY = usePillFollow ? kSlidePillSplitY : kSlideSplitY;
  if (kind == TransKind::SLIDE_LEFT || kind == TransKind::SLIDE_RIGHT) {
    // Keep top status/clock band fixed from destination frame during horizontal slides.
    memcpy(fb, to, (size_t)slideSplitY * UI_W * sizeof(uint16_t));
  }

  const int64_t tStartUs = esp_timer_get_time();
  const int64_t durUs    = (int64_t)ms * 1000LL;

  while (durUs > 0) {
    int64_t elapsedUs = esp_timer_get_time() - tStartUs;
    if (elapsedUs >= durUs) break;
    if (elapsedUs < 0) elapsedUs = 0;

    float u = (float)elapsedUs / (float)durUs;
    // 进入类模态用 outQuart（收尾更柔）；水平滑屏默认 180ms outCubic，启用 NOVA_SLIDE_EASE_QUART 切至 200ms outQuart。
#ifdef NOVA_SLIDE_EASE_QUART
    const bool useQuart = (kind == TransKind::POP_IN ||
                           kind == TransKind::SLIDE_LEFT || kind == TransKind::SLIDE_RIGHT);
#else
    const bool useQuart = (kind == TransKind::POP_IN);
#endif
    float ease = useQuart ? ease::outQuart(u) : ease::outCubic(u);

    switch (kind) {
      case TransKind::SLIDE_LEFT:
      case TransKind::SLIDE_RIGHT: {
        int offset = (int)(ease * UI_W + 0.5f);
        if (offset < 0) offset = 0;
        if (offset > UI_W) offset = UI_W;
        int keepFrom = UI_W - offset;
        bool enterFromRight = (kind == TransKind::SLIDE_LEFT);
        for (int y = slideSplitY; y < UI_H; y++) {
          uint16_t* dstRow = fb + (size_t)y * UI_W;
          const uint16_t* srcFrom = from + (size_t)y * UI_W;
          const uint16_t* srcTo   = to   + (size_t)y * UI_W;
          if (enterFromRight) {
            if (keepFrom > 0) {
              memcpy(dstRow, srcFrom + offset, (size_t)keepFrom * sizeof(uint16_t));
            }
            if (offset > 0) {
              memcpy(dstRow + keepFrom, srcTo, (size_t)offset * sizeof(uint16_t));
            }
          } else {
            if (offset > 0) {
              memcpy(dstRow, srcTo + keepFrom, (size_t)offset * sizeof(uint16_t));
            }
            if (keepFrom > 0) {
              memcpy(dstRow + offset, srcFrom, (size_t)keepFrom * sizeof(uint16_t));
            }
          }
        }
        if (usePillFollow) {
          ui::drawPagePillsSlide(canvas, (int16_t)(UI_W / 2 + pills->ox),
                                 (int16_t)(52 + pills->oy),
                                 pills->fromDot, pills->toDot, pills->totalDots, u);
        }
        break;
      }
      case TransKind::SHEET_UP: {
        int offset = (int)(ease * UI_H + 0.5f);
        if (offset < 0) offset = 0;
        if (offset > UI_H) offset = UI_H;
        int splitY = UI_H - offset;
        int32_t scale = 256 - (int32_t)(ease * (float)kDimSpan + 0.5f);
        if (scale < kDimMinScale) scale = kDimMinScale;
        if (scale > 256) scale = 256;
        if (splitY > 0) dimRows(fb, from, splitY, scale);
        if (offset > 0) {
          memcpy(fb + (size_t)splitY * UI_W, to, (size_t)offset * UI_W * sizeof(uint16_t));
        }
        break;
      }
      case TransKind::SHEET_DOWN: {
        int offset = (int)(ease * UI_H + 0.5f);
        if (offset < 0) offset = 0;
        if (offset > UI_H) offset = UI_H;
        int remainFrom = UI_H - offset;
        int32_t scale = kDimMinScale + (int32_t)(ease * (float)kDimSpan + 0.5f);
        if (scale < kDimMinScale) scale = kDimMinScale;
        if (scale > 256) scale = 256;
        if (offset > 0) dimRows(fb, to, offset, scale);
        if (remainFrom > 0) {
          memcpy(fb + (size_t)offset * UI_W, from,
                 (size_t)remainFrom * UI_W * sizeof(uint16_t));
        }
        break;
      }
      case TransKind::FADE: {
        int alpha5 = (int)(ease * 32.0f + 0.5f);
        if (alpha5 < 0) alpha5 = 0;
        if (alpha5 > 32) alpha5 = 32;
        blendFrame(fb, from, to, pxCount, (uint32_t)alpha5);
        break;
      }
      case TransKind::POP_IN:
      case TransKind::POP_OUT: {
        int alpha5 = (int)(ease * 32.0f + 0.5f);
        if (alpha5 < 0) alpha5 = 0;
        if (alpha5 > 32) alpha5 = 32;
        const bool popIn = (kind == TransKind::POP_IN);
        // POP_IN：目标帧整体下移 off 行后逐渐归位；POP_OUT：源帧逐渐下沉 off 行。
        // 逐行取偏移后的源行再做与 FADE 相同的 alpha 混合，开销与 FADE 持平。
        const int off = popIn ? (int)((1.0f - ease) * kPopRisePx + 0.5f)
                              : (int)(ease * kPopSinkPx + 0.5f);
        for (int y = 0; y < UI_H; y++) {
          int fy = y, ty = y;
          if (popIn) {
            ty = y - off;          // 目标帧内容出现在下方 off 行处
            if (ty < 0) ty = 0;    // 顶部空出的行复用第 0 行（纯背景）
          } else {
            fy = y - off;
            if (fy < 0) fy = 0;
          }
          blendFrame(fb + (size_t)y * UI_W, from + (size_t)fy * UI_W,
                     to + (size_t)ty * UI_W, UI_W, (uint32_t)alpha5);
        }
        break;
      }
    }

    hwDisplayPush();
  }

  memcpy(fb, to, fbBytes);
  hwDisplayPush();
#endif
}
