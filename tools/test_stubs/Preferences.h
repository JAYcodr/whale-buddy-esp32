#pragma once
// Host-test stub for ESP32 Preferences: keeps the same typed-getter
// semantics (getUChar only reads U8, getUShort only U16, getBool reads U8,
// getString only STR; a type mismatch returns the default).
#include "Arduino.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

namespace prefs_stub {

enum class T : uint8_t { NONE, U8, U16, U32, I8, STR };

struct Entry {
  T type = T::NONE;
  uint32_t num = 0;
  std::string str;
};

inline std::map<std::string, Entry>& store() {
  static std::map<std::string, Entry> s;
  return s;
}

inline void clear() { store().clear(); }

inline void setU8(const char* k, uint8_t v) { store()[k] = Entry{ T::U8, v, {} }; }
inline void setU16(const char* k, uint16_t v) { store()[k] = Entry{ T::U16, v, {} }; }
inline void setU32(const char* k, uint32_t v) { store()[k] = Entry{ T::U32, v, {} }; }
inline void setI8(const char* k, int8_t v) {
  store()[k] = Entry{ T::I8, (uint32_t)(uint8_t)v, {} };
}
inline void setStr(const char* k, const char* v) { store()[k] = Entry{ T::STR, 0, v }; }

inline Entry get(const char* k) {
  auto it = store().find(k);
  return (it == store().end()) ? Entry{} : it->second;
}

}  // namespace prefs_stub

class Preferences {
 public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}

  uint8_t getUChar(const char* key, uint8_t def) const {
    const auto e = prefs_stub::get(key);
    return (e.type == prefs_stub::T::U8) ? (uint8_t)e.num : def;
  }
  uint16_t getUShort(const char* key, uint16_t def) const {
    const auto e = prefs_stub::get(key);
    return (e.type == prefs_stub::T::U16) ? (uint16_t)e.num : def;
  }
  bool getBool(const char* key, bool def) const {
    return getUChar(key, def ? 1 : 0) == 1;
  }
  String getString(const char* key, const String& def) const {
    const auto e = prefs_stub::get(key);
    return (e.type == prefs_stub::T::STR) ? String(e.str) : def;
  }

  size_t putUChar(const char* key, uint8_t v) {
    prefs_stub::setU8(key, v);
    return 1;
  }
  size_t putUShort(const char* key, uint16_t v) {
    prefs_stub::setU16(key, v);
    return 2;
  }
  size_t putBool(const char* key, bool v) { return putUChar(key, v ? 1 : 0); }
  size_t putString(const char* key, const char* v) {
    prefs_stub::setStr(key, v);
    return std::strlen(v) + 1;
  }
  size_t putString(const char* key, const String& v) { return putString(key, v.c_str()); }
};
