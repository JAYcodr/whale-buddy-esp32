#pragma once
#include <stdint.h>
#include "model/battery_model.h"

constexpr const char* NOVA_FW_VER = "0.4.0";

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
  HomePageKind homePage      = HomePageKind::PET;
  PowerProfile powerProfile  = PowerProfile::ADAPTIVE;
  uint8_t      soundPct      = 75;
  uint8_t      brightLevel   = 4;
  bool         useNva        = true;
  bool         ttsEnabled    = true;
  bool         quietHours    = true;
  bool         choreoEnabled = true;
  uint16_t     batSleepSec   = 30;
  uint8_t      chargeLimitPct = 90;                  // 80 / 90 / 100
  ChargeSpeed  chargeSpeed    = ChargeSpeed::AUTO;   // 温和 / 自动 / 快速
  char         brokerHost[40] = "192.168.31.15";
  uint16_t     brokerPort    = 1883;
};

void settingsLoad();
void settingsSave();
const NovaSettings& settingsGet();
NovaSettings& settingsMut();
uint8_t settingsEffectiveFps(bool usbPresent);
