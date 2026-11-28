#include "ui/pages.h"
#include "model/settings.h"

namespace {

struct ActionMap {
  UiAction::Kind kind;
  uint8_t idx;   // kSettings 的下标
};

// 下标必须与 settings.h 里 kSettings[] 的顺序一致。
constexpr ActionMap kActionMap[] = {
  { UiAction::SET_HOME,      0 },
  { UiAction::SET_POWER,     1 },
  { UiAction::SET_SOUND,     2 },
  { UiAction::SET_BRIGHT,    3 },
  { UiAction::SET_TTS,       4 },
  { UiAction::SET_QUIET,     5 },
  { UiAction::SET_CHOREO,    6 },
  { UiAction::SET_BATSLEEP,  7 },
  { UiAction::SET_CHGLIMIT,  8 },
  { UiAction::SET_CHGSPEED,  9 },
  { UiAction::SET_AOD_MODE,  10 },
  { UiAction::SET_AOD_IDLE,  11 },
  { UiAction::SET_AOD_NIGHT, 12 },
  { UiAction::SET_MIC_GAIN,  13 },
  { UiAction::SET_THEME,     14 },
};

}  // namespace

bool settingsApplyAction(UiAction::Kind kind, int value) {
  for (const ActionMap& m : kActionMap) {
    if (m.kind != kind) continue;
    const SettingDef& d = kSettings[m.idx];
    settingsFieldSet(settingsMut(), d, settingsSanitize(d, (value < 0) ? 0 : (uint16_t)value));
    settingsSave();
    return true;
  }
  return false;
}
