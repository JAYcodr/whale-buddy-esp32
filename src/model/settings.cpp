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

void settingsLoad() {
#ifndef NOVA_HOST_SIM
  Preferences prefs;
  if (prefs.begin("nova", true)) {
    uint8_t hp = prefs.getUChar("home", 0);
    s_cfg.homePage = (hp <= 2) ? (HomePageKind)hp : HomePageKind::PET;
    uint8_t pp = prefs.getUChar("pwr", 0);
    s_cfg.powerProfile = (pp <= 2) ? (PowerProfile)pp : PowerProfile::ADAPTIVE;
    s_cfg.soundPct = prefs.getUChar("snd", 75);
    if (s_cfg.soundPct > 100) s_cfg.soundPct = 100;
    s_cfg.brightLevel = prefs.getUChar("brt", 4);
    if (s_cfg.brightLevel > 4) s_cfg.brightLevel = 4;
    s_cfg.useNva = prefs.getBool("nva", true);
    s_cfg.ttsEnabled = prefs.getBool("tts", true);
    s_cfg.quietHours = prefs.getBool("quiet", true);
    s_cfg.choreoEnabled = prefs.getBool("choreo", true);
    uint16_t slp = prefs.getUShort("sleep_s", 30);
    s_cfg.batSleepSec = (slp >= 10 && slp <= 600) ? slp : 30;
    uint8_t lim = prefs.getUChar("chg_lim", 90);
    s_cfg.chargeLimitPct = (lim == 80 || lim == 90 || lim == 100) ? lim : 90;
    uint8_t spd = prefs.getUChar("chg_spd", (uint8_t)ChargeSpeed::AUTO);
    s_cfg.chargeSpeed = (spd <= (uint8_t)ChargeSpeed::FAST)
                          ? (ChargeSpeed)spd : ChargeSpeed::AUTO;
    uint8_t aodm = prefs.getUChar("aod", aod::USB);
    s_cfg.aodMode = (aodm <= aod::ALWAYS) ? aodm : aod::USB;
    uint16_t aodIdle = prefs.getUShort("aod_idle", 120);
    s_cfg.aodIdleSec = (aodIdle == 60 || aodIdle == 120 || aodIdle == 300) ? aodIdle : 120;
    uint8_t aodNight = prefs.getUChar("aod_night", 1);
    s_cfg.aodNightCap = (aodNight <= 2) ? aodNight : 1;
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
    prefs.putUChar("home", (uint8_t)s_cfg.homePage);
    prefs.putUChar("pwr", (uint8_t)s_cfg.powerProfile);
    prefs.putUChar("snd", s_cfg.soundPct);
    prefs.putUChar("brt", s_cfg.brightLevel);
    prefs.putBool("nva", s_cfg.useNva);
    prefs.putBool("tts", s_cfg.ttsEnabled);
    prefs.putBool("quiet", s_cfg.quietHours);
    prefs.putBool("choreo", s_cfg.choreoEnabled);
    prefs.putUShort("sleep_s", s_cfg.batSleepSec);
    prefs.putUChar("chg_lim", s_cfg.chargeLimitPct);
    prefs.putUChar("chg_spd", (uint8_t)s_cfg.chargeSpeed);
    prefs.putUChar("aod", s_cfg.aodMode);
    prefs.putUShort("aod_idle", s_cfg.aodIdleSec);
    prefs.putUChar("aod_night", s_cfg.aodNightCap);
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
