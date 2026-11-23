#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM (1u << 10)
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT (1u << 2)
#endif

inline void* heap_caps_malloc(size_t size, uint32_t) { return malloc(size); }
