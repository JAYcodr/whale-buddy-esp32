#pragma once
#include <stdint.h>

// LVGL-style 4-bpp (16 alpha levels) anti-aliased bitmap font.
// Packed 2 pixels per byte: high nibble = even x, low nibble = odd x.

struct AaGlyph {
  uint16_t cp;       // Unicode BMP codepoint (sorted ascending)
  uint8_t  w;
  uint8_t  h;
  uint32_t dataOff;  // Byte offset inside font data[]
  uint8_t  xAdv;
  int8_t   xOff;
  int8_t   yOff;     // Signed offset from baseline (negative = above baseline)
};

struct AaFont {
  uint8_t        lineHeight;
  uint8_t        ascent;
  uint8_t        descent;
  uint16_t       glyphCount;
  const AaGlyph* glyphs;
  const uint8_t* data;
  const AaFont*  fallback;
};
