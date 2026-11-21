#pragma once
#include <stdint.h>

enum class HomePageKind : uint8_t {
  PET   = 0,
  QUOTA = 1,
  RANK  = 2,
};

enum class PowerProfile : uint8_t {
  ADAPTIVE  = 0,  // USB 60fps / Battery 30fps
  LONG_BATT = 1,  // Battery 15fps + aggressive idle
  LIVELY    = 2,  // Always 60fps
};

struct NovaSettings {
  HomePageKind homePage     = HomePageKind::PET;
  PowerProfile powerProfile = PowerProfile::ADAPTIVE;
  uint8_t      soundPct     = 75;
  uint8_t      brightLevel  = 4;
  bool         useNva       = true;
  char         brokerHost[40] = "192.168.31.15";
  uint16_t     brokerPort   = 1883;
};

void settingsLoad();
void settingsSave();
const NovaSettings& settingsGet();
NovaSettings& settingsMut();
uint8_t settingsEffectiveFps(bool usbPresent);
