#pragma once
#include <stdint.h>

struct HwBattery {
  int   mV;          // battery voltage, millivolts
  int   mA;          // + discharging, − charging (AXP2101 has no current ADC)
  int   pct;         // 0..100, estimated (see model/battery_model.h)
  bool  usbPresent;  // VBUS > 4V
  bool  charging;
  int   tempC;       // AXP2101 die temperature
  uint8_t  stage;    // BattStage (model/battery_model.h)
  uint16_t chgMa;    // charge current the governor asked for (0 = paused)
  bool     chargeLimited;  // paused by limit/temperature
  int8_t   gaugePct; // AXP2101 fuel-gauge SOC, -1 when unavailable
};

bool hwPowerInit();

// Cached snapshot — never touches I2C. Updated by hwPowerTick().
HwBattery hwBattery();

// 1 s tick from the bus task: read the PMU, run the estimator and the charge
// governor. Kept out of hwBattery() so a BLE/status read can't advance time.
void hwPowerTick(uint32_t nowMs);

// Screen state for the charge governor (bright screen -> lower charge current).
void hwPowerSetScreenOn(bool on);

void hwPowerOff();
// AXP2101 power-key IRQ helpers. Each call reads + clears the IRQ
// flag, returning true at most once per physical press. Use these
// instead of raw getIrqStatus() — the AXP register bit positions
// don't match the XPOWERS_PWR_BTN_* enum values.
bool hwAxpPekeyShortPress();
bool hwAxpPekeyLongPress();

#include <XPowersLib.h>
XPowersPMU* hwPmuRef();   // raw access for boards that need direct register / rail control
