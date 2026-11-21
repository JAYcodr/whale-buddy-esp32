#include "audio/sfx.h"
#include "hw/audio.h"

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
