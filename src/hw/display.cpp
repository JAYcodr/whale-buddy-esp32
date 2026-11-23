#include "hw/display.h"
#include "hw/pins.h"
#include <Arduino.h>
#include <Wire.h>

static Arduino_DataBus*  s_bus    = nullptr;
static Arduino_OLED*     s_gfx    = nullptr;
static Arduino_Canvas*   s_canvas = nullptr;

static bool i2cAck(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission(true) == 0;
}


#if BOARD_DISPLAY_SH8601_VENDOR_INIT
// LVGL demo SH8601 init for the 2.16 panel revision.
// Source: ~/Downloads/ESP32-C6-Touch-AMOLED-2.16/02_Example/Arduino-v3.3.3/08_LVGL_V8_Test/bsp_lvgl_port.cpp:25-41
// Re-runs after Arduino_SH8601's basic init to override pixel format,
// MADCTL, gamma / power-related vendor commands, set the column/row
// range to the full 480×480, push brightness to max, and re-issue
// DISPON. Without this the panel inits but stays dark.
static void sh8601_vendor_init(Arduino_DataBus* bus) {
  static const uint8_t init_ops[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,            // SLPOUT (re-issue, harmless)
    END_WRITE,
    DELAY, 120,
    BEGIN_WRITE,
    WRITE_C8_D8, 0xFE, 0x20,          // page select MFR
    WRITE_C8_D8, 0x19, 0x10,
    WRITE_C8_D8, 0x1C, 0xA0,
    WRITE_C8_D8, 0xFE, 0x00,          // back to USER page
    WRITE_C8_D8, 0xC4, 0x80,
    WRITE_C8_D8, 0x3A, 0x55,          // pixel format (override 0x05 → 0x55)
    WRITE_C8_D8, 0x35, 0x00,          // tearing line
    WRITE_C8_D8, 0x36, 0x30,          // MADCTL (override lib's 0x00)
    WRITE_C8_D8, 0x53, 0x20,          // CABC / brightness control
    WRITE_C8_D8, 0x51, 0xFF,          // brightness max
    WRITE_C8_D8, 0x63, 0xFF,
    WRITE_COMMAND_8, 0x2A, WRITE_BYTES, 4, 0x00, 0x00, 0x01, 0xDF,  // col 0..479
    WRITE_COMMAND_8, 0x2B, WRITE_BYTES, 4, 0x00, 0x00, 0x01, 0xDF,  // row 0..479
    WRITE_COMMAND_8, 0x29,            // DISPON (re-issue with full config)
    END_WRITE,
    DELAY, 50,
  };
  bus->batchOperation(init_ops, sizeof(init_ops));
}
#endif

bool hwDisplayInit() {
  s_bus = new Arduino_ESP32QSPI(
    PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0, PIN_LCD_SDIO1,
    PIN_LCD_SDIO2, PIN_LCD_SDIO3);
#if BOARD_DISPLAY_CO5300
  // CO5300 ctor: (bus, rst, rotation, w, h, col_off1, row_off1, col_off2, row_off2)
  // col_offset1 = 6 on round 466×466 (1.75c), 0 on rounded-square 480×480 (S3-2.16).
  // Pass PIN_LCD_RESET so the driver does its own 200 ms hardware reset —
  // the 20 ms pulse from hwExpanderResetSequence() is too short for CO5300.
  s_gfx = new Arduino_CO5300(s_bus, PIN_LCD_RESET, BOARD_DISPLAY_ROTATION,
                             LCD_W_PHYS, LCD_H_PHYS, BOARD_CO5300_COL_OFFSET, 0, 0, 0);
#else
  if (!i2cAck(0x38) && i2cAck(0x15)) {
    // Later Waveshare 1.8" batches swap FT3168/SH8601 for CST820/CO5300 (+16px X offset).
    Serial.println("hwDisplay: detected CST820@0x15 -> CO5300(+16px)");
    s_gfx = new Arduino_CO5300(s_bus, GFX_NOT_DEFINED, 0, LCD_W_PHYS, LCD_H_PHYS, 16, 0, 0, 0);
  } else {
    s_gfx = new Arduino_SH8601(s_bus, GFX_NOT_DEFINED, 0, LCD_W_PHYS, LCD_H_PHYS);
  }
#endif
  s_canvas = new Arduino_Canvas(HW_W, HW_H, s_gfx);
  // canvas->begin() internally calls gfx->begin() which calls bus init.
  // Calling them separately would double-init the SPI bus → ESP_ERR_INVALID_STATE.
#ifndef BOARD_DISPLAY_QSPI_HZ
#define BOARD_DISPLAY_QSPI_HZ GFX_NOT_DEFINED
#endif
  if (!s_canvas->begin(BOARD_DISPLAY_QSPI_HZ)) { Serial.println("hwDisplay: canvas begin failed"); return false; }
  // UTF-8 decode for u8g2 CJK fonts — without this, print() treats each
  // byte of a multi-byte codepoint as its own glyph lookup → mojibake.
  s_canvas->setUTF8Print(true);
#if BOARD_DISPLAY_CO5300 && (BOARD_CO5300_MADCTL != 0)
  // CO5300 panels where the natural orientation needs row/column swap
  // (MADCTL bit MV=0x20) — Arduino_CO5300's setRotation only does X/Y
  // mirror, so write MADCTL directly here. 0x60 = MV+MX (90° CW), 0xA0 = MV+MY (90° CCW).
  s_bus->beginWrite();
  s_bus->writeC8D8(0x36, BOARD_CO5300_MADCTL);
  s_bus->endWrite();
#endif
#if BOARD_DISPLAY_SH8601_VENDOR_INIT
  sh8601_vendor_init(s_bus);
  // The vendor init ends with brightness max (0x51 0xFF) and DISPON.
  // Black-fill the whole panel once so the borders aren't whatever junk
  // the controller had at power-up.
  s_gfx->fillScreen(0x0000);
#else
  s_gfx->setBrightness(0);   // black first frame to avoid white flash
  delay(20);
  s_gfx->setBrightness(150);  // default mid-brightness; main may override later
#endif
  return true;
}

Arduino_Canvas* hwCanvas() { return s_canvas; }

void hwDisplayBrightness(uint8_t pct) {
  // 20..100 → 50..255：20% 对齐老固件最暗档（50/255），往上是连续的 205 级。
  if (pct < 20) pct = 20;
  if (pct > 100) pct = 100;
  s_gfx->setBrightness((uint8_t)(50u + (uint16_t)(pct - 20u) * 205u / 80u));
}

void hwDisplaySleep(bool off) {
  if (off) {
    s_gfx->setBrightness(0);
    s_gfx->displayOff();
  } else {
    s_gfx->displayOn();
    hwDisplayBrightness(60);  // mid; caller usually re-applies stored level
  }
}

static uint16_t s_lineBuf[LCD_W_PHYS];   // one physical row, internal RAM
static bool     s_borderAlertOn = false;

extern "C" void hwBorderAlertSetInternal(bool on);  // forward decl from border.cpp

void hwBorderAlertSetInternal(bool on) { s_borderAlertOn = on; }

static const uint16_t BORDER_RED = 0xF800;

void hwDisplayPush() {
#ifdef DEBUG_SAFE_BOX
  // 1-px green outline at the logical safe-area boundary. Used to
  // empirically tune SAFE_INSET against the physical rounded bezel.
  s_canvas->drawRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, 0x07E0);
#endif
  uint16_t* src = (uint16_t*)s_canvas->getFramebuffer();
#if BOARD_DISPLAY_LETTERBOX
  // Bilinear scale HW_W×HW_H → DEST_W×DEST_H, centred in physical frame,
  // surround black. One-shot draw16bitRGBBitmap is required on CO5300 — per-row
  // draws leave the screen black (QSPI state issue when chaining many writes).
  //
  // RGB565 trick: split R|B (mask 0xF81F) and G (mask 0x07E0) into separate
  // 32-bit accumulators so one multiply-add handles both channels in parallel.
  // Weight is 5-bit (0..32); two successive blends (horizontal then vertical)
  // divide by 32×32 = 1024.
  static uint16_t* s_frameBuf = nullptr;
  if (!s_frameBuf) {
    s_frameBuf = (uint16_t*)heap_caps_malloc(
        LCD_W_PHYS * LCD_H_PHYS * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frameBuf) { Serial.println("hwDisplay: frameBuf alloc failed"); return; }
    memset(s_frameBuf, 0, LCD_W_PHYS * LCD_H_PHYS * sizeof(uint16_t));
  }
  constexpr int DEST_W = BOARD_DISPLAY_DEST_W;
  constexpr int DEST_H = BOARD_DISPLAY_DEST_H;
  constexpr int OFF_X  = (LCD_W_PHYS - DEST_W) / 2;
  constexpr int OFF_Y  = (LCD_H_PHYS - DEST_H) / 2;
  // Fixed-point scale: top 16 bits = integer source coord, next 5 = fraction.
  constexpr uint32_t SCALE_X = ((uint32_t)HW_W << 16) / DEST_W;
  constexpr uint32_t SCALE_Y = ((uint32_t)HW_H << 16) / DEST_H;

  for (int dy = 0; dy < DEST_H; dy++) {
    uint32_t sy_fp = (uint32_t)dy * SCALE_Y;
    int y0 = sy_fp >> 16;
    int y1 = (y0 + 1 < HW_H) ? y0 + 1 : y0;
    uint32_t fy = (sy_fp >> 11) & 0x1F;
    uint32_t inv_fy = 32 - fy;

    uint16_t* row0 = src + y0 * HW_W;
    uint16_t* row1 = src + y1 * HW_W;
    uint16_t* dstRow = s_frameBuf + (dy + OFF_Y) * LCD_W_PHYS + OFF_X;

    for (int dx = 0; dx < DEST_W; dx++) {
      uint32_t sx_fp = (uint32_t)dx * SCALE_X;
      int x0 = sx_fp >> 16;
      int x1 = (x0 + 1 < HW_W) ? x0 + 1 : x0;
      uint32_t fx = (sx_fp >> 11) & 0x1F;
      uint32_t inv_fx = 32 - fx;

      uint16_t p00 = row0[x0];
      uint16_t p01 = row0[x1];
      uint16_t p10 = row1[x0];
      uint16_t p11 = row1[x1];

      uint32_t t_rb = (p00 & 0xF81F) * inv_fx + (p01 & 0xF81F) * fx;
      uint32_t t_g  = (p00 & 0x07E0) * inv_fx + (p01 & 0x07E0) * fx;
      uint32_t b_rb = (p10 & 0xF81F) * inv_fx + (p11 & 0xF81F) * fx;
      uint32_t b_g  = (p10 & 0x07E0) * inv_fx + (p11 & 0x07E0) * fx;

      uint32_t rb = (t_rb * inv_fy + b_rb * fy) >> 10;
      uint32_t g  = (t_g  * inv_fy + b_g  * fy) >> 10;

      dstRow[dx] = (uint16_t)((rb & 0xF81F) | (g & 0x07E0));
    }
  }
  s_gfx->draw16bitRGBBitmap(0, 0, s_frameBuf, LCD_W_PHYS, LCD_H_PHYS);
#else
#if BOARD_DISPLAY_PUSH_STREAMED
  // Streamed 2× upscale: one continuous QSPI transaction.
  // CS stays asserted across all rows so the panel never sees bus idle
  // (which causes per-row draws to fail on this 2.16 panel revision).
  s_gfx->startWrite();
  s_gfx->writeAddrWindow(BOARD_DISPLAY_OFFSET_X, BOARD_DISPLAY_OFFSET_Y,
                         BOARD_HW_W * 2, BOARD_HW_H * 2);
  for (int y = 0; y < HW_H; y++) {
    uint16_t* row = src + y * HW_W;
    for (int x = 0; x < HW_W; x++) {
      // Byte-swap on the way in: writeBytes() emits raw bytes, but the
      // SH8601 expects MSB-first RGB565 per MIPI DCS. draw16bitRGBBitmap()
      // does this swap internally; writeBytes() doesn't.
      uint16_t c = row[x];
      uint16_t s = (uint16_t)((c >> 8) | (c << 8));
      s_lineBuf[x*2]     = s;
      s_lineBuf[x*2 + 1] = s;
    }
    // Each canvas row writes twice for 2× vertical expansion.
    // Width is HW_W*2 px = 368 px = 736 bytes.
    s_gfx->writeBytes((uint8_t*)s_lineBuf, HW_W * 2 * 2);
    s_gfx->writeBytes((uint8_t*)s_lineBuf, HW_W * 2 * 2);
  }
  s_gfx->endWrite();
#elif BOARD_DISPLAY_SCALE == 1
  // Native 1:1 blit via hwDisplayPushRect (zero-copy full-width fast path).
  hwDisplayPushRect(0, 0, HW_W, HW_H);
  return;
#else
  // 2× integer upscale, per-row, with optional centring offset.
  // OFFSET_X/Y are 0 on full-fill boards (1.8).
  for (int y = 0; y < HW_H; y++) {
    uint16_t* row = src + y * HW_W;
    for (int x = 0; x < HW_W; x++) {
      uint16_t c = row[x];
      s_lineBuf[x*2]     = c;
      s_lineBuf[x*2 + 1] = c;
    }
    int dy = y * 2 + BOARD_DISPLAY_OFFSET_Y;
    s_gfx->draw16bitRGBBitmap(BOARD_DISPLAY_OFFSET_X, dy,     s_lineBuf, HW_W*2, 1);
    s_gfx->draw16bitRGBBitmap(BOARD_DISPLAY_OFFSET_X, dy + 1, s_lineBuf, HW_W*2, 1);
  }
#endif
#endif

  // Attention indicator: small red pill centered at the top of the
  // panel. Inset well below the rounded bezel corners. Less intrusive
  // than a full frame; reads as a "notification dot".
  if (s_borderAlertOn) {
    const int BAR_W = 200;
    const int BAR_H = 8;
    const int BAR_Y = 18;
    const int BAR_X = (LCD_W_PHYS - BAR_W) / 2;
    s_gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, BORDER_RED);
  }
}

void hwDisplayPushRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!s_canvas || !s_gfx || w <= 0 || h <= 0) return;

#if BOARD_DISPLAY_SCALE == 1 && !BOARD_DISPLAY_LETTERBOX && !BOARD_DISPLAY_PUSH_STREAMED
  int16_t x0 = x < 0 ? 0 : x;
  int16_t y0 = y < 0 ? 0 : y;
  int16_t x1 = (x + w) > HW_W ? HW_W : (x + w);
  int16_t y1 = (y + h) > HW_H ? HW_H : (y + h);
  if (x1 <= x0 || y1 <= y0) return;

  // SH8601 QSPI requires even col/row windows and width > 16 to avoid column drop.
  x0 &= ~1;
  x1 = (x1 + 1) & ~1;
  if (x1 > HW_W) x1 = HW_W;
  if (x1 - x0 < 18) {
    x1 = x0 + 18;
    if (x1 > HW_W) {
      x1 = HW_W;
      x0 = (x1 - 18) & ~1;
      if (x0 < 0) x0 = 0;
    }
  }
  y0 &= ~1;
  y1 = (y1 + 1) & ~1;
  if (y1 > HW_H) y1 = HW_H;

  const int16_t rw = x1 - x0;
  const int16_t rh = y1 - y0;
  uint16_t* src = (uint16_t*)s_canvas->getFramebuffer();
  if (!src) return;

  // Wide regions (>= 240 px): push full-width band zero-copy from framebuffer,
  // avoiding slow PSRAM-to-PSRAM row copies.
  if (rw >= 240) {
    x0 = 0;
    s_gfx->draw16bitRGBBitmap(BOARD_DISPLAY_OFFSET_X, y0 + BOARD_DISPLAY_OFFSET_Y,
                              src + (int32_t)y0 * HW_W, HW_W, rh);
  } else {
    static uint16_t* s_fastRectBuf = nullptr;   // 64 KB internal DMA-capable SRAM strip buffer
    if (!s_fastRectBuf) {
      s_fastRectBuf = (uint16_t*)heap_caps_malloc(65536, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_fastRectBuf) {
      int16_t stripRows = (int16_t)(65536 / ((size_t)rw * sizeof(uint16_t))) & ~1;
      if (stripRows < 2) stripRows = 2;
      for (int16_t sy = 0; sy < rh; sy += stripRows) {
        int16_t sh = rh - sy;
        if (sh > stripRows) sh = stripRows;
        if ((sh & 1) && (sy + sh < rh)) sh--;
        for (int16_t row = 0; row < sh; row++) {
          memcpy(s_fastRectBuf + (int32_t)row * rw,
                 src + (int32_t)(y0 + sy + row) * HW_W + x0,
                 (size_t)rw * sizeof(uint16_t));
        }
        s_gfx->draw16bitRGBBitmap(x0 + BOARD_DISPLAY_OFFSET_X, y0 + sy + BOARD_DISPLAY_OFFSET_Y,
                                  s_fastRectBuf, rw, sh);
      }
    } else {
      s_gfx->draw16bitRGBBitmap(BOARD_DISPLAY_OFFSET_X, y0 + BOARD_DISPLAY_OFFSET_Y,
                                src + (int32_t)y0 * HW_W, HW_W, rh);
    }
  }

  if (s_borderAlertOn) {
    const int BAR_W = 200;
    const int BAR_H = 8;
    const int BAR_Y = 18;
    const int BAR_X = (LCD_W_PHYS - BAR_W) / 2;
    if (y0 < BAR_Y + BAR_H && y1 > BAR_Y) {
      s_gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, BORDER_RED);
    }
  }
#else
  (void)x; (void)y; (void)w; (void)h;
  hwDisplayPush();
#endif
}
