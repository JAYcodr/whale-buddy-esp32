#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))
#endif

#define F(x) (x)
#define HIGH 1
#define LOW 0

using std::abs;
using std::max;
using std::min;
using String = std::string;

inline void yield() {}
inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}

#include "Print.h"
