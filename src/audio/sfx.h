#pragma once
#include <stdint.h>

enum class SfxKind : uint8_t {
  NAV_CHIRP = 0,
  NEW_EVENT,
  APPROVAL_ARRIVE,
  ACCEPT,
  DENY,
  LOW_BATT,
  SETTING_OK,
};

void sfxPlay(SfxKind kind, uint8_t volPct = 100);
void sfxPlayPet(SfxKind kind, uint8_t volPct, uint8_t petKind);
void sfxPlayClawdReaction(uint8_t animKey, uint8_t volPct, uint8_t hour, bool timeValid, bool respectQuiet = true);
void sfxPlayClawdEvent(const char* evt, uint8_t volPct, uint8_t hour, bool timeValid, bool respectQuiet = true);
bool sfxSpeakFile(const char* path, uint8_t volPct, uint8_t hour, bool timeValid, bool respectQuiet = true);
