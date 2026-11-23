#pragma once
// Host-test stub for Arduino.h: only what settings.cpp + log.h need.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using String = std::string;

struct SerialStub {
  template <typename... Args>
  int printf(const char*, Args...) { return 0; }
};

inline SerialStub Serial;
