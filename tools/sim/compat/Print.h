#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class __FlashStringHelper;

class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    while (size--) {
      if (write(*buffer++)) n++;
      else break;
    }
    return n;
  }

  size_t print(const char* s) {
    return s ? write((const uint8_t*)s, std::strlen(s)) : 0;
  }
  size_t print(const __FlashStringHelper* s) {
    return print((const char*)s);
  }
  size_t print(const std::string& s) {
    return write((const uint8_t*)s.data(), s.size());
  }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) {
    char b[32];
    int n = std::snprintf(b, sizeof(b), "%d", v);
    return n > 0 ? write((const uint8_t*)b, (size_t)n) : 0;
  }
  size_t print(unsigned int v) {
    char b[32];
    int n = std::snprintf(b, sizeof(b), "%u", v);
    return n > 0 ? write((const uint8_t*)b, (size_t)n) : 0;
  }
  size_t println(const char* s = "") {
    size_t n = print(s);
    n += write('\n');
    return n;
  }
  size_t printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    return write((const uint8_t*)buf, len);
  }
};
