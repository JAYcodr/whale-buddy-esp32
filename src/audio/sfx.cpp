#include "audio/sfx.h"
#include "hw/audio.h"
#include "model/aod_policy.h"
#include <LittleFS.h>

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
  }
}

bool sfxSpeakFile(const char* path, uint8_t volPct, uint8_t hour, bool timeValid, bool respectQuiet) {
  if (volPct == 0 || isQuietTime(hour, timeValid, respectQuiet) || !path || !path[0]) return false;
  if (!LittleFS.exists(path)) return false;
  hwPlayAdpcmFile(path, volPct);
  return true;
}
