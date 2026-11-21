#pragma once

#ifndef U8G2_WITH_UNICODE
#define U8G2_WITH_UNICODE 1
#endif

#ifndef U8G2_FONT_SECTION
#define U8G2_FONT_SECTION(name)
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern const uint8_t u8g2_font_wqy14_t_gb2312[];
extern const uint8_t u8g2_font_wqy16_t_gb2312[];
extern const uint8_t u8g2_font_helvB14_tf[];
extern const uint8_t u8g2_font_helvB18_tf[];
extern const uint8_t u8g2_font_logisoso24_tf[];
#ifdef __cplusplus
}
#endif
