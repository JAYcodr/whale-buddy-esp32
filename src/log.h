#pragma once
#include <Arduino.h>
enum class LogLvl : uint8_t { I = 0, W = 1, E = 2 };
#ifndef NOVA_LOG_MIN
#define NOVA_LOG_MIN 1          // 默认 WARN
#endif
#define NOVA_LOG(lvl, tag, ...) do { \
  if ((int)LogLvl::lvl >= NOVA_LOG_MIN) Serial.printf("[" tag "] " __VA_ARGS__); \
} while (0)
