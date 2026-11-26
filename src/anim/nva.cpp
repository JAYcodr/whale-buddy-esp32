#include "anim/nva.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NOVA_HOST_SIM
#include <zlib.h>
#else
#include <Arduino.h>
#include <esp_timer.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <miniz.h>
#endif

namespace {

// 解码耗时累计（微秒）：UI 任务每秒取一次差值，折算 decBusyPct。
// 只累计 inflate + XOR 这段真正吃 CPU 的工作，不把锁等待算进去。
uint32_t s_decodeBusyUs = 0;

inline uint32_t busyNowUs() {
#ifndef NOVA_HOST_SIM
  return (uint32_t)esp_timer_get_time();
#else
  return 0;
#endif
}

constexpr const char* ANIM_NAMES[(int)NvaAnimKey::COUNT] = {
  "idle", "thinking", "working", "waiting", "success", "error", "click", "sleep", "quota",
  "click_happy", "click_shy", "click_angry", "click_giggle",
  "crab_walk", "happy_run", "coding", "archive",
  "stretch", "yawn", "wake_up", "hum", "eat_token", "bubble", "quota_worry",
};

// All NVA2 packs are baked at uniform world scale from the 640x360 source stage,
// so runtime base scale is 1:1 (no nearest-neighbor resize blur).
constexpr float kAnimBaseScale[(int)NvaAnimKey::COUNT] = {
  1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f,
  1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f,
  1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f,
};

// Resident keys stay cached in PSRAM; others load on demand into a shared buffer.
bool isResident(NvaAnimKey k) {
  return k == NvaAnimKey::IDLE || k == NvaAnimKey::SUCCESS || k == NvaAnimKey::ERROR;
}

struct NvaPack {
  bool     valid = false;
  bool     isV2 = false;
  uint32_t genId = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  uint16_t nframes = 0;
  uint8_t  ncolors = 0;
  uint8_t  fps = 10;
  uint16_t pal565[256] = {};
  uint32_t* offsets = nullptr;   // nframes + 1 entries
  uint8_t*  blobs = nullptr;     // raw compressed blob data
  size_t    blobsLen = 0;
  uint8_t*  rawFile = nullptr;   // whole .nva file in PSRAM/heap
  size_t    rawLen = 0;
};

char    s_rootDir[64] = "/pets/nova";
bool    s_ready = false;
NvaPack s_resPacks[3];         // IDLE, SUCCESS, ERROR
NvaPack s_streamPack;          // shared on-demand slot
NvaAnimKey s_streamKey = NvaAnimKey::COUNT;
uint32_t s_packGenSeq = 1;

volatile NvaAnimKey s_desiredKey = NvaAnimKey::IDLE;
volatile bool       s_desiredOnce = false;
volatile uint32_t   s_desiredRestartGen = 0;

NvaAnimKey s_activeKey  = NvaAnimKey::IDLE;
bool       s_activeOnce = false;
uint32_t   s_seenRestartGen = 0;
uint16_t   s_frameIdx   = 0;
uint8_t    s_activeFps  = 10;
uint32_t   s_nextFrameMs = 0;
volatile bool     s_clipEnded = false;
volatile uint32_t s_loopCycles = 0;

// Double-buffered decoded index frame (0 = transparent, 1..ncolors = palette).
constexpr size_t MAX_FRAME_PIXELS = 220 * 224;
uint8_t*  s_frameBuf[2] = { nullptr, nullptr };
uint8_t*  s_xorScratch  = nullptr;
uint8_t   s_frontIdx = 0;
uint16_t  s_frontW = 0, s_frontH = 0;
uint16_t  s_frontPal[256] = {};
NvaAnimKey s_frontKey = NvaAnimKey::IDLE;
bool      s_frontReady = false;
uint32_t  s_lastPackGen = 0;
uint16_t  s_lastFrameIdx = 0;

#ifndef NOVA_HOST_SIM
SemaphoreHandle_t s_mtx = nullptr;
TaskHandle_t      s_decodeTask = nullptr;
volatile bool     s_needDecode = false;
#endif

uint16_t readU16Le(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t readU32Le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void freePack(NvaPack& pk) {
  if (pk.rawFile) {
    free(pk.rawFile);
    pk.rawFile = nullptr;
  }
  if (pk.offsets) {
    free(pk.offsets);
    pk.offsets = nullptr;
  }
  pk.blobs = nullptr;
  pk.blobsLen = 0;
  pk.rawLen = 0;
  pk.valid = false;
  pk.isV2 = false;
  pk.genId = 0;
}

uint8_t* allocBuffer(size_t bytes, bool preferPsram) {
#ifdef NOVA_HOST_SIM
  (void)preferPsram;
  return (uint8_t*)malloc(bytes);
#else
  if (preferPsram) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return (uint8_t*)p;
  }
  return (uint8_t*)malloc(bytes);
#endif
}

bool parsePack(NvaPack& pk) {
  if (!pk.rawFile || pk.rawLen < 12) return false;
  const uint8_t* d = pk.rawFile;
  if (memcmp(d, "NVA2", 4) == 0) {
    pk.isV2 = true;
  } else if (memcmp(d, "NVA1", 4) == 0) {
    pk.isV2 = false;
  } else {
    return false;
  }

  pk.w       = readU16Le(d + 4);
  pk.h       = readU16Le(d + 6);
  pk.nframes = readU16Le(d + 8);
  pk.ncolors = d[10];
  pk.fps     = d[11] > 0 ? d[11] : 10;

  if (pk.w == 0 || pk.h == 0 || pk.nframes == 0) return false;
  if ((size_t)pk.w * pk.h > MAX_FRAME_PIXELS) return false;

  const size_t palOff  = 12;
  const size_t offOff  = palOff + (size_t)pk.ncolors * 3;
  const size_t dataOff = offOff + (size_t)(pk.nframes + 1) * 4;
  if (pk.rawLen < dataOff) return false;

  pk.pal565[0] = 0;
  for (uint16_t i = 0; i < pk.ncolors; i++) {
    uint8_t r = d[palOff + i * 3 + 0];
    uint8_t g = d[palOff + i * 3 + 1];
    uint8_t b = d[palOff + i * 3 + 2];
    pk.pal565[i + 1] = rgb565(r, g, b);
  }

  pk.offsets = (uint32_t*)allocBuffer((size_t)(pk.nframes + 1) * sizeof(uint32_t), true);
  if (!pk.offsets) return false;
  for (uint16_t i = 0; i <= pk.nframes; i++) {
    pk.offsets[i] = readU32Le(d + offOff + i * 4);
  }
  pk.blobs    = pk.rawFile + dataOff;
  pk.blobsLen = pk.rawLen - dataOff;
  if (pk.offsets[pk.nframes] > pk.blobsLen) return false;

  pk.genId = s_packGenSeq++;
  pk.valid = true;
  return true;
}

bool loadPackFromFs(NvaAnimKey key, NvaPack& pk) {
  freePack(pk);
  char path[96];
  snprintf(path, sizeof(path), "%s/%s.nva", s_rootDir, ANIM_NAMES[(int)key]);

#ifdef NOVA_HOST_SIM
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return false; }
  pk.rawFile = allocBuffer((size_t)sz, true);
  if (!pk.rawFile) { fclose(f); return false; }
  pk.rawLen = fread(pk.rawFile, 1, (size_t)sz, f);
  fclose(f);
#else
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz == 0) { f.close(); return false; }
  pk.rawFile = allocBuffer(sz, true);
  if (!pk.rawFile) { f.close(); return false; }
  pk.rawLen = f.read(pk.rawFile, sz);
  f.close();
#endif

  if (pk.rawLen != (size_t)sz || !parsePack(pk)) {
    freePack(pk);
    return false;
  }
  return true;
}

int residentSlotIndex(NvaAnimKey k) {
  if (k == NvaAnimKey::IDLE)    return 0;
  if (k == NvaAnimKey::SUCCESS) return 1;
  if (k == NvaAnimKey::ERROR)   return 2;
  return -1;
}

NvaPack* acquirePack(NvaAnimKey k) {
  int ridx = residentSlotIndex(k);
  if (ridx >= 0) {
    return s_resPacks[ridx].valid ? &s_resPacks[ridx] : nullptr;
  }
  if (s_streamKey == k && s_streamPack.valid) {
    return &s_streamPack;
  }
  if (loadPackFromFs(k, s_streamPack)) {
    s_streamKey = k;
    return &s_streamPack;
  }
  return s_resPacks[0].valid ? &s_resPacks[0] : nullptr;
}

bool inflateZlib(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstBytes) {
#ifdef NOVA_HOST_SIM
  uLongf outLen = (uLongf)dstBytes;
  if (uncompress(dst, &outLen, src, (uLong)srcLen) != Z_OK) return false;
  return outLen == dstBytes;
#else
  static tinfl_decompressor* s_decomp = nullptr;
  if (!s_decomp) {
    s_decomp = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
    if (!s_decomp) return false;
  }
  size_t inBytes = srcLen;
  size_t outLen = dstBytes;
  tinfl_init(s_decomp);
  tinfl_status status = tinfl_decompress(
      s_decomp, src, &inBytes, dst, dst, &outLen,
      TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return status == TINFL_STATUS_DONE && outLen == dstBytes;
#endif
}

bool decodeFrameToBack(const NvaPack& pk, uint16_t frameIdx, NvaAnimKey key) {
  if (!pk.valid || frameIdx >= pk.nframes) return false;
  const uint32_t sOff = pk.offsets[frameIdx];
  const uint32_t eOff = pk.offsets[frameIdx + 1];
  if (eOff <= sOff || eOff > pk.blobsLen) return false;

  const size_t pixBytes = (size_t)pk.w * pk.h;
  const uint8_t backIdx = s_frontIdx ^ 1;
  uint8_t* dst = s_frameBuf[backIdx];
  if (!dst) return false;

  const uint8_t* blobPtr = pk.blobs + sOff;
  size_t blobLen = (size_t)(eOff - sOff);
  bool isDelta = false;
  if (pk.isV2) {
    if (blobLen < 2) return false;
    isDelta = (blobPtr[0] == 1);
    blobPtr += 1;
    blobLen -= 1;
  }

  const uint32_t busyT0 = busyNowUs();
  if (isDelta) {
    if (!s_xorScratch || !s_frontReady || s_lastPackGen != pk.genId || s_lastFrameIdx + 1 != frameIdx) {
      return false;
    }
    if (!inflateZlib(blobPtr, blobLen, s_xorScratch, pixBytes)) return false;
    const uint8_t* prev = s_frameBuf[s_frontIdx];
    for (size_t i = 0; i < pixBytes; i++) {
      dst[i] = prev[i] ^ s_xorScratch[i];
    }
  } else {
    if (!inflateZlib(blobPtr, blobLen, dst, pixBytes)) return false;
  }
  s_decodeBusyUs += busyNowUs() - busyT0;

#ifndef NOVA_HOST_SIM
  xSemaphoreTake(s_mtx, portMAX_DELAY);
#endif
  s_frontIdx = backIdx;
  s_frontW   = pk.w;
  s_frontH   = pk.h;
  memcpy(s_frontPal, pk.pal565, (size_t)(pk.ncolors + 1) * sizeof(uint16_t));
  s_frontKey = key;
  s_lastPackGen = pk.genId;
  s_lastFrameIdx = frameIdx;
  s_frontReady = true;
#ifndef NOVA_HOST_SIM
  xSemaphoreGive(s_mtx);
#endif
  return true;
}

#ifndef NOVA_HOST_SIM
void decodeTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!s_needDecode) continue;
    s_needDecode = false;

    NvaAnimKey target = s_desiredKey;
    bool targetOnce = s_desiredOnce;
    uint32_t restartGen = s_desiredRestartGen;
    bool switching = (target != s_activeKey || restartGen != s_seenRestartGen);

    if (switching) {
      s_activeKey = target;
      s_activeOnce = targetOnce;
      s_seenRestartGen = restartGen;
      s_frameIdx = 0;
      s_clipEnded = false;
      NvaPack* pk = acquirePack(s_activeKey);
      if (pk && pk->valid) {
        s_activeFps = pk->fps > 0 ? pk->fps : 10;
        decodeFrameToBack(*pk, 0, s_activeKey);
      }
      continue;
    }

    s_activeOnce = targetOnce;
    if (s_activeOnce && s_clipEnded) continue;

    NvaPack* pk = acquirePack(s_activeKey);
    if (pk && pk->valid && pk->nframes > 0) {
      s_activeFps = pk->fps > 0 ? pk->fps : 10;
      if (s_activeOnce && (uint32_t)(s_frameIdx + 1) >= pk->nframes) {
        if (target == s_desiredKey && restartGen == s_desiredRestartGen) {
          s_clipEnded = true;
          s_loopCycles++;
        }
        continue;
      }
      uint16_t nextIdx = (uint16_t)((s_frameIdx + 1) % pk->nframes);
      s_frameIdx = nextIdx;
      decodeFrameToBack(*pk, s_frameIdx, s_activeKey);
      if (nextIdx == 0 && target == s_desiredKey && restartGen == s_desiredRestartGen) {
        s_loopCycles++;
      }
    }
  }
}
#endif

}  // namespace

bool nvaInit(const char* rootDir) {
  if (rootDir && rootDir[0]) {
    strncpy(s_rootDir, rootDir, sizeof(s_rootDir) - 1);
    s_rootDir[sizeof(s_rootDir) - 1] = '\0';
  }

#ifndef NOVA_HOST_SIM
  if (!LittleFS.begin(false)) {
    Serial.println("nva: LittleFS mount failed");
    return false;
  }
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
#endif

  s_frameBuf[0] = allocBuffer(MAX_FRAME_PIXELS, false);
  s_frameBuf[1] = allocBuffer(MAX_FRAME_PIXELS, false);
  s_xorScratch  = allocBuffer(MAX_FRAME_PIXELS, true);
  if (!s_frameBuf[0] || !s_frameBuf[1] || !s_xorScratch) return false;

  bool okIdle = loadPackFromFs(NvaAnimKey::IDLE, s_resPacks[0]);
  loadPackFromFs(NvaAnimKey::SUCCESS, s_resPacks[1]);
  loadPackFromFs(NvaAnimKey::ERROR, s_resPacks[2]);
  s_ready = okIdle;
  if (!s_ready) return false;

  s_activeKey = NvaAnimKey::IDLE;
  s_desiredKey = NvaAnimKey::IDLE;
  s_frameIdx = 0;
  decodeFrameToBack(s_resPacks[0], 0, NvaAnimKey::IDLE);
  s_nextFrameMs = 0;

#ifndef NOVA_HOST_SIM
  if (!s_decodeTask) {
    xTaskCreatePinnedToCore(decodeTaskFn, "task_decode", 8192, nullptr, 3, &s_decodeTask, 1);
  }
#endif
  return true;
}

bool nvaAvailable() { return s_ready; }

void nvaSetState(NvaAnimKey key, bool once, bool restart) {
  if ((uint8_t)key >= (uint8_t)NvaAnimKey::COUNT) return;
  if (s_desiredKey == key && s_desiredOnce == once && !restart) return;
  s_desiredKey = key;
  s_desiredOnce = once;
  if (restart || s_desiredKey != s_activeKey) {
    s_clipEnded = false;
  }
  if (restart) {
    s_desiredRestartGen++;
  }
  s_nextFrameMs = 0;
#ifndef NOVA_HOST_SIM
  s_needDecode = true;
  if (s_decodeTask) xTaskNotifyGive(s_decodeTask);
#else
  if (s_desiredKey != s_activeKey || s_desiredRestartGen != s_seenRestartGen) {
    s_activeKey = s_desiredKey;
    s_activeOnce = s_desiredOnce;
    s_seenRestartGen = s_desiredRestartGen;
    s_frameIdx  = 0;
    s_clipEnded = false;
    NvaPack* pk = acquirePack(s_activeKey);
    if (pk && pk->valid) {
      s_activeFps = pk->fps > 0 ? pk->fps : 10;
      decodeFrameToBack(*pk, s_frameIdx, s_activeKey);
    }
  }
#endif
}

NvaAnimKey nvaCurrentState() { return s_desiredKey; }
NvaAnimKey nvaActiveState() {
#ifndef NOVA_HOST_SIM
  if (s_mtx) {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    NvaAnimKey k = s_frontKey;
    xSemaphoreGive(s_mtx);
    return k;
  }
#endif
  return s_frontKey;
}
bool nvaIsEnded() { return s_clipEnded; }
uint32_t nvaLoopCycles() { return s_loopCycles; }
uint32_t nvaDecodeBusyUs() { return s_decodeBusyUs; }
int16_t nvaFrameWidth()  { return (s_ready && s_frontReady) ? (int16_t)s_frontW : 144; }
int16_t nvaFrameHeight() { return (s_ready && s_frontReady) ? (int16_t)s_frontH : 162; }

void nvaTick(uint32_t nowMs) {
  if (!s_ready) return;
  if (s_activeOnce && s_clipEnded && s_desiredOnce && s_desiredKey == s_activeKey) return;
  uint8_t fps = s_activeFps > 0 ? s_activeFps : 10;
  uint32_t stepMs = 1000 / fps;
  if (s_nextFrameMs == 0) s_nextFrameMs = nowMs;
  if ((int32_t)(nowMs - s_nextFrameMs) < 0) return;

  s_nextFrameMs += stepMs;
  if ((int32_t)(nowMs - s_nextFrameMs) > (int32_t)(stepMs * 2)) {
    s_nextFrameMs = nowMs + stepMs;
  }
#ifndef NOVA_HOST_SIM
  s_needDecode = true;
  if (s_decodeTask) xTaskNotifyGive(s_decodeTask);
#else
  bool switching = (s_desiredKey != s_activeKey || s_desiredRestartGen != s_seenRestartGen);
  if (switching) {
    s_activeKey = s_desiredKey;
    s_activeOnce = s_desiredOnce;
    s_seenRestartGen = s_desiredRestartGen;
    s_frameIdx  = 0;
    s_clipEnded = false;
    NvaPack* pk = acquirePack(s_activeKey);
    if (pk && pk->valid) {
      s_activeFps = pk->fps > 0 ? pk->fps : 10;
      decodeFrameToBack(*pk, s_frameIdx, s_activeKey);
    }
    return;
  }
  s_activeOnce = s_desiredOnce;
  NvaPack* pk = acquirePack(s_activeKey);
  if (pk && pk->valid && pk->nframes > 0) {
    s_activeFps = pk->fps > 0 ? pk->fps : 10;
    if (s_activeOnce && (uint32_t)(s_frameIdx + 1) >= pk->nframes) {
      s_clipEnded = true;
      s_loopCycles++;
      return;
    }
    s_frameIdx = (uint16_t)((s_frameIdx + 1) % pk->nframes);
    decodeFrameToBack(*pk, s_frameIdx, s_activeKey);
    if (s_frameIdx == 0) s_loopCycles++;
  }
#endif
}

bool nvaDraw(NovaCanvas& c, int16_t centerX, int16_t bottomY, bool flipX, float scaleY) {
  if (!s_ready || !s_frontReady) return false;
#ifndef NOVA_HOST_SIM
  xSemaphoreTake(s_mtx, portMAX_DELAY);
#endif
  const uint8_t* src = s_frameBuf[s_frontIdx];
  const int16_t w = (int16_t)s_frontW;
  const int16_t h = (int16_t)s_frontH;
  const float baseScale = ((uint8_t)s_frontKey < (uint8_t)NvaAnimKey::COUNT)
                            ? kAnimBaseScale[(uint8_t)s_frontKey]
                            : 1.0f;
  uint16_t* fb = ((Arduino_Canvas&)c).getFramebuffer();
  const int16_t fbW = c.width();
  const int16_t fbH = c.height();

  if (fb) {
    bool unitBase = (baseScale > 0.995f && baseScale < 1.005f);
    bool unitSquash = (scaleY > 0.995f && scaleY < 1.005f);
    if (!flipX && unitBase && unitSquash) {
      const int16_t x0 = centerX - w / 2;
      const int16_t y0 = bottomY - h;
      for (int16_t row = 0; row < h; row++) {
        const int16_t dy = y0 + row;
        if (dy < 0 || dy >= fbH) continue;
        const uint8_t* srow = src + (int32_t)row * w;
        uint16_t* drow = fb + (int32_t)dy * fbW;
        for (int16_t col = 0; col < w; col++) {
          uint8_t idx = srow[col];
          if (idx) {
            const int16_t dx = x0 + col;
            if (dx >= 0 && dx < fbW) drow[dx] = s_frontPal[idx];
          }
        }
      }
    } else {
      if (scaleY < 0.78f) scaleY = 0.78f;
      if (scaleY > 1.14f) scaleY = 1.14f;
      float squashX = unitSquash ? 1.0f : (1.0f / sqrtf(scaleY));
      if (squashX < 0.88f) squashX = 0.88f;
      if (squashX > 1.16f) squashX = 1.16f;
      float totalX = baseScale * squashX;
      float totalY = baseScale * scaleY;

      const int16_t dstW = (int16_t)(w * totalX + 0.5f);
      const int16_t dstH = (int16_t)(h * totalY + 0.5f);
      if (dstW > 0 && dstH > 0) {
        const int16_t x0 = centerX - dstW / 2;
        const int16_t y0 = bottomY - dstH;
        const int32_t stepX = ((int32_t)w << 16) / dstW;
        const int32_t stepY = ((int32_t)h << 16) / dstH;

        for (int16_t row = 0; row < dstH; row++) {
          const int16_t dy = y0 + row;
          if (dy < 0 || dy >= fbH) continue;
          int16_t sy = (int16_t)(((int32_t)row * stepY) >> 16);
          if (sy >= h) sy = h - 1;
          const uint8_t* srow = src + (int32_t)sy * w;
          uint16_t* drow = fb + (int32_t)dy * fbW;
          for (int16_t col = 0; col < dstW; col++) {
            const int16_t dx = x0 + col;
            if (dx < 0 || dx >= fbW) continue;
            int16_t sx = (int16_t)(((int32_t)col * stepX) >> 16);
            if (sx >= w) sx = w - 1;
            uint8_t idx = srow[flipX ? (w - 1 - sx) : sx];
            if (idx) drow[dx] = s_frontPal[idx];
          }
        }
      }
    }
  }
#ifndef NOVA_HOST_SIM
  xSemaphoreGive(s_mtx);
#endif
  return true;
}
