#pragma once
#include <stdint.h>

enum class SfxKind : uint8_t {
  NAV_CHIRP = 0,
  NEW_EVENT,
  APPROVAL_ARRIVE,
  ACCEPT,
  DENY,
  LOW_BATT,
};

void sfxPlay(SfxKind kind, uint8_t volPct = 100);
bool sfxSpeakFile(const char* path, uint8_t volPct, uint8_t hour, bool timeValid);
