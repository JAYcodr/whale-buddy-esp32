#include "audio/sfx.h"
#include "anim/nva.h"
#include "hw/audio.h"
#include "model/aod_policy.h"
#include <LittleFS.h>
#include <string.h>

static bool isQuietTime(uint8_t hour, bool timeValid, bool respectQuiet) {
  return respectQuiet && timeValid && aod::isQuietHour(hour);
}

void sfxPlay(SfxKind kind, uint8_t volPct) {
  if (volPct == 0) return;
  switch (kind) {
    case SfxKind::NAV_CHIRP:
      hwTone(980, 1280, 40, volPct);
      break;
    case SfxKind::NEW_EVENT:
      hwTone(587, 587, 55, volPct);
      hwTone(880, 880, 75, volPct);
      break;
    case SfxKind::APPROVAL_ARRIVE:
      hwTone(659, 659, 60, volPct);
      hwTone(784, 784, 60, volPct);
      hwTone(1047, 1047, 90, volPct);
      break;
    case SfxKind::ACCEPT:
      hwTone(523, 1047, 110, volPct);
      break;
    case SfxKind::DENY:
      hwTone(523, 294, 130, volPct);
      break;
    case SfxKind::LOW_BATT:
      hwTone(440, 440, 70, volPct);
      hwTone(330, 330, 110, volPct);
      break;
    case SfxKind::SETTING_OK:
      hwTone(784, 784, 38, volPct);
      hwTone(1175, 1175, 58, volPct);
      break;
  }
}

void sfxPlayPet(SfxKind kind, uint8_t volPct, uint8_t petKind) {
  if (volPct == 0) return;
  if (petKind == 0) {
    sfxPlay(kind, volPct);
    return;
  }
  // Clawd：8-bit 电子游戏风格短音（纯 hwTone 合成，不走女生人声）
  switch (kind) {
    case SfxKind::NAV_CHIRP:
      hwTone(1047, 1397, 28, volPct);
      break;
    case SfxKind::NEW_EVENT:
      hwTone(659, 659, 42, volPct);
      hwTone(988, 988, 56, volPct);
      break;
    case SfxKind::APPROVAL_ARRIVE:
      hwTone(880, 880, 50, volPct);
      hwTone(1175, 1175, 50, volPct);
      hwTone(1568, 1568, 78, volPct);
      break;
    case SfxKind::ACCEPT:
      hwTone(523, 523, 42, volPct);
      hwTone(659, 659, 42, volPct);
      hwTone(784, 784, 42, volPct);
      hwTone(1047, 1047, 82, volPct);
      break;
    case SfxKind::DENY:
      hwTone(370, 277, 70, volPct);
      hwTone(277, 165, 105, volPct);
      break;
    case SfxKind::LOW_BATT:
      hwTone(440, 392, 65, volPct);
      hwTone(330, 220, 105, volPct);
      break;
    case SfxKind::SETTING_OK:
      hwTone(784, 784, 34, volPct);
      hwTone(1568, 1568, 54, volPct);
      break;
  }
}

void sfxPlayClawdReaction(uint8_t animKey, uint8_t volPct, uint8_t hour, bool timeValid, bool respectQuiet) {
  if (volPct == 0 || isQuietTime(hour, timeValid, respectQuiet)) return;
  switch ((NvaAnimKey)animKey) {
    case NvaAnimKey::WAKE_UP:
      hwTone(659, 659, 45, volPct);
      hwTone(1319, 1319, 95, volPct);
      break;
    case NvaAnimKey::CLICK_ANGRY:
      hwTone(440, 294, 52, volPct);
      hwTone(330, 196, 75, volPct);
      break;
    case NvaAnimKey::CLICK_SHY:
    case NvaAnimKey::CLICK_GIGGLE:
      hwTone(587, 880, 44, volPct);
      hwTone(784, 1175, 58, volPct);
      break;
    case NvaAnimKey::CLICK_HAPPY:
    case NvaAnimKey::CLICK:
    default:
      hwTone(988, 988, 44, volPct);
      hwTone(1319, 1319, 78, volPct);
      break;
  }
}

void sfxPlayClawdEvent(const char* evt, uint8_t volPct, uint8_t hour, bool timeValid, bool respectQuiet) {
  if (volPct == 0 || isQuietTime(hour, timeValid, respectQuiet) || !evt) return;
  if (strcmp(evt, "boot") == 0 || strcmp(evt, "done") == 0) {
    sfxPlayPet(SfxKind::ACCEPT, volPct, 1);
  } else if (strcmp(evt, "error") == 0) {
    sfxPlayPet(SfxKind::DENY, volPct, 1);
  } else if (strcmp(evt, "wake") == 0) {
    sfxPlayClawdReaction((uint8_t)NvaAnimKey::WAKE_UP, volPct, hour, timeValid, respectQuiet);
  } else {
    sfxPlayPet(SfxKind::NEW_EVENT, volPct, 1);
  }
}

bool sfxSpeakFile(const char* path, uint8_t volPct, uint8_t hour, bool timeValid, bool respectQuiet) {
  if (volPct == 0 || isQuietTime(hour, timeValid, respectQuiet) || !path || !path[0]) return false;
  if (!LittleFS.exists(path)) return false;
  hwPlayAdpcmFile(path, volPct);
  return true;
}
