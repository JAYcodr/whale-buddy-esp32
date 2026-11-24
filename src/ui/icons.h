#pragma once
#include <stdint.h>

// 4-bpp anti-aliased UI icons generated from MIT/ISC Lucide SVGs (tools/icons/gen_icons.py).
// Packed 2 pixels per byte: high nibble = even x, low nibble = odd x.
struct AaIcon {
  uint8_t        w;
  uint8_t        h;
  const uint8_t* data;
};

extern const AaIcon kIconMic18;
extern const AaIcon kIconCheck12;
extern const AaIcon kIconX12;
extern const AaIcon kIconPlay12;
extern const AaIcon kIconChevronRight12;
extern const AaIcon kIconCircleDashed14;
