#include "model/settings.h"
#include <string.h>

#include "log.h"

#ifndef NOVA_HOST_SIM
#include <Preferences.h>
#endif

namespace {
NovaSettings s_cfg;
}

static uint32_t s_nvsFails = 0;

uint16_t settingsFieldGet(const NovaSettings& cfg, const SettingDef& d) {
  const char* p = reinterpret_cast<const char*>(&cfg) + d.offset;
  switch (d.kind) {
    case SettingKind::BOOL: return *reinterpret_cast<const bool*>(p) ? 1 : 0;
    case SettingKind::U16:  return *reinterpret_cast<const uint16_t*>(p);
    case SettingKind::U8:
    case SettingKind::CHOICE: return *reinterpret_cast<const uint8_t*>(p);
  }
  return 0;
}

void settingsFieldSet(NovaSettings& cfg, const SettingDef& d, uint16_t v) {
  char* p = reinterpret_cast<char*>(&cfg) + d.offset;
  switch (d.kind) {
    case SettingKind::BOOL: *reinterpret_cast<bool*>(p) = (v != 0); break;
    case SettingKind::U16:  *reinterpret_cast<uint16_t*>(p) = v; break;
    case SettingKind::U8:
    case SettingKind::CHOICE: *reinterpret_cast<uint8_t*>(p) = (uint8_t)v; break;
  }
}

namespace {

#ifndef NOVA_HOST_SIM
uint16_t settingSanitize(const SettingDef& d, uint16_t v) {
  if (d.choices && d.nChoices) {
    for (uint8_t i = 0; i < d.nChoices; i++) {
      if (d.choices[i] == v) return v;
    }
    return d.def;
  }
  if (v < d.minV) return d.def;
  if (v > d.maxV) return d.clampMax ? d.maxV : d.def;
  return v;
}
#endif

}  // namespace

void settingsLoad() {
#ifndef NOVA_HOST_SIM
  Preferences prefs;
  if (prefs.begin("nova", true)) {
    for (const SettingDef& d : kSettings) {
      uint16_t raw = d.def;
      switch (d.kind) {
        case SettingKind::BOOL:
          raw = prefs.getBool(d.key, d.def != 0) ? 1 : 0;
          break;
        case SettingKind::U16:
          raw = prefs.getUShort(d.key, d.def);
          break;
        case SettingKind::U8:
        case SettingKind::CHOICE:
          raw = prefs.getUChar(d.key, (uint8_t)d.def);
          break;
      }
      settingsFieldSet(s_cfg, d, settingSanitize(d, raw));
    }
    String host = prefs.getString("mqtt_host", "192.168.31.15");
    strncpy(s_cfg.brokerHost, host.c_str(), sizeof(s_cfg.brokerHost) - 1);
    s_cfg.brokerHost[sizeof(s_cfg.brokerHost) - 1] = '\0';
    uint16_t port = prefs.getUShort("mqtt_port", 1883);
    s_cfg.brokerPort = (port > 0) ? port : 1883;
    prefs.end();
  } else {
    s_nvsFails++;
    NOVA_LOG(E, "nvs", "prefs open failed\n");
  }
#endif
}

void settingsSave() {
#ifndef NOVA_HOST_SIM
  Preferences prefs;
  if (prefs.begin("nova", false)) {
    for (const SettingDef& d : kSettings) {
      const uint16_t v = settingsFieldGet(s_cfg, d);
      switch (d.kind) {
        case SettingKind::BOOL:
          prefs.putBool(d.key, v != 0);
          break;
        case SettingKind::U16:
          prefs.putUShort(d.key, v);
          break;
        case SettingKind::U8:
        case SettingKind::CHOICE:
          prefs.putUChar(d.key, (uint8_t)v);
          break;
      }
    }
    prefs.putString("mqtt_host", s_cfg.brokerHost);
    prefs.putUShort("mqtt_port", s_cfg.brokerPort);
    prefs.end();
  }
#endif
}

const NovaSettings& settingsGet() { return s_cfg; }
NovaSettings& settingsMut() { return s_cfg; }

uint8_t settingsEffectiveFps(bool usbPresent) {
  switch (s_cfg.powerProfile) {
    case PowerProfile::LONG_BATT: return usbPresent ? 30 : 15;
    case PowerProfile::LIVELY:    return 60;
    case PowerProfile::ADAPTIVE:
    default:                      return usbPresent ? 60 : 30;
  }
}

uint32_t settingsNvsFailCount() { return s_nvsFails; }
