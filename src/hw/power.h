#pragma once
#include "model/battery_model.h"
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
  bool     chargeLimited;     // paused by limit/temperature
  bool     chargeTempCapped;  // paused specifically due to die temperature
  int8_t   gaugePct;          // AXP2101 fuel-gauge SOC, -1 when unavailable
  uint8_t  rawPct;            // IR-compensated OCV pct (diagnostic)
  uint16_t compMv;            // IR-compensated mV (diagnostic)
  uint16_t effMa;             // modeled effective cell current (diagnostic)
  uint16_t bootMv;            // unloaded boot voltage sample (diagnostic)
  int8_t   seedPct;           // boot seed pct (-1 = none)
  bool     seedPowerLost;     // PMU lost power before this boot
  bool     vbusLimited;       // AXP2101 VBUS input current DPM active
  uint16_t vbusMv;            // AXP2101 VBUS ADC voltage, millivolts
  uint16_t vsysMv;            // AXP2101 VSYS ADC voltage, millivolts
  bool     thermReg;          // AXP2101 hardware thermal regulation active
  uint8_t  pwrOnSrc;          // AXP2101 PWRON_STATUS (0x20) latched at boot
  uint8_t  pwrOffSrc;         // AXP2101 PWROFF_STATUS (0x21) latched at boot
};

bool hwPowerInit();

// Cached snapshot — never touches I2C. Updated by hwPowerTick().
HwBattery hwBattery();
BatteryCal hwBatteryCal();
PowerWatermark hwPowerWatermark();
bool hwPowerPeekEvent(PowerEvent* out);
void hwPowerAckEvent();
void hwPowerResetCal();

// 1 s tick from the bus task: read the PMU, run the estimator and the charge
// governor. Kept out of hwBattery() so a BLE/status read can't advance time.
void hwPowerTick(uint32_t nowMs);

// Screen / AOD state for charge governor & per-session power watermarks.
void hwPowerSetScreenOn(bool on);
void hwPowerSetAodActive(bool active);

void hwPowerOff();
// AXP2101 power-key IRQ helpers. Each call reads + clears the IRQ
// flag, returning true at most once per physical press. Use these
// instead of raw getIrqStatus() — the AXP register bit positions
// don't match the XPOWERS_PWR_BTN_* enum values.
bool hwAxpPekeyShortPress();
bool hwAxpPekeyLongPress();

#include <XPowersLib.h>
XPowersPMU* hwPmuRef();   // raw access for boards that need direct register / rail control
