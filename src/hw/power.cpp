#include "hw/power.h"
#include "hw/pins.h"
#include "model/settings.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <math.h>
#include <freertos/FreeRTOS.h>

namespace {

XPowersPMU s_pmu;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
HwBattery   s_bat = {};

BatteryModel    s_model;
ChargeGovernor  s_gov;
uint32_t        s_lastTickMs = 0;
bool            s_tickInit   = false;
volatile bool   s_screenOn   = true;

// Applied charge config, so the governor only touches I2C when it changes.
bool     s_appliedEnable = true;
uint16_t s_appliedMa     = 300;
uint8_t  s_appliedCurr   = 0xFF;

uint8_t currEnum(uint16_t ma) {
  if (ma <= 100) return XPOWERS_AXP2101_CHG_CUR_100MA;
  if (ma <= 125) return XPOWERS_AXP2101_CHG_CUR_125MA;
  if (ma <= 150) return XPOWERS_AXP2101_CHG_CUR_150MA;
  if (ma <= 175) return XPOWERS_AXP2101_CHG_CUR_175MA;
  if (ma <= 200) return XPOWERS_AXP2101_CHG_CUR_200MA;
  if (ma <= 300) return XPOWERS_AXP2101_CHG_CUR_300MA;
  if (ma <= 400) return XPOWERS_AXP2101_CHG_CUR_400MA;
  return XPOWERS_AXP2101_CHG_CUR_500MA;
}

void applyCharging(bool enable, uint16_t ma) {
  if (!enable) {
    if (s_appliedEnable) {
      s_pmu.disableCellbatteryCharge();
      s_appliedEnable = false;
      s_appliedMa     = 0;
      Serial.println("power: charge paused");
    }
    return;
  }
  if (s_appliedEnable && ma == s_appliedMa) return;

  const uint8_t e = currEnum(ma);
  if (e != s_appliedCurr) {
    s_pmu.setChargerConstantCurr(e);
    s_appliedCurr = e;
  }
  if (!s_appliedEnable) {
    s_pmu.enableCellbatteryCharge();
    Serial.println("power: charge resumed");
  }
  s_appliedEnable = true;
  s_appliedMa     = ma;
}

}  // namespace

bool hwPowerInit() {
  if (!s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("hwPower: AXP2101 begin failed");
    return false;
  }

  // Charge profile for the 600mAh pack. 300mA is 0.5C; the governor moves it
  // between 100 and 500mA afterwards depending on screen state and die temp.
  s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_300MA);
  s_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  s_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_50MA);
  s_pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_500MA);
  // Leave setLowBatShutdownThreshold at its reset value: it keys off the
  // fuel gauge, which is not calibrated for this pack yet. A surprise hard
  // power-off at "5%" would be worse than the current soft-empty anchor.
  s_appliedCurr = XPOWERS_AXP2101_CHG_CUR_300MA;

  s_pmu.enableBattDetection();
  s_pmu.enableVbusVoltageMeasure();
  s_pmu.enableBattVoltageMeasure();
  s_pmu.enableSystemVoltageMeasure();
  s_pmu.enableTemperatureMeasure();

  // Fuel gauge (REG 0x18 bit3). XPowersLib dropped enableGauge() in this
  // version, but the bit is the same one the official board example sets.
  s_pmu.setRegisterBit(XPOWERS_AXP2101_CHARGE_GAUGE_WDT_CTRL, 3);

  // ALDO3 → display rail on all three boards. enableALDO3() is idempotent.
  s_pmu.enableALDO3();

#if BOARD_AXP_ENABLE_AUX_LDOS
  // 2.16: ALDO2 powers DSI_PWR_EN (display power-enable signal via R13);
  // ALDO1/4 power MIC bias and secondary sensor rails. Without this the
  // panel stays dark even with ALDO3 enabled (DSI_PWR_EN floats).
  // Voltages match the XiaoZhi 2.16 reference: ALDO1..4 = 3.3V, DCDC1 = 3.3V.
  s_pmu.setDC1Voltage(3300);
  s_pmu.setALDO1Voltage(3300);
  s_pmu.setALDO2Voltage(3300);
  s_pmu.setALDO4Voltage(3300);
  s_pmu.enableALDO1();
  s_pmu.enableALDO2();
  s_pmu.enableALDO4();
#endif

#if BOARD_AXP_PWRON_4S_OFF
  // 2.16: configure AXP to power off on 4 s PWRON-hold (the PWR key is
  // the only software-configurable shutdown path on this board).
  // 0x22 reg: PWRON > OFFLEVEL as POWEROFF source. 0x27 reg: 4s timing.
  s_pmu.writeRegister(0x22, 0b110);
  s_pmu.writeRegister(0x27, 0x10);
#endif

  s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  // AXP2101::enableIRQ() takes chip-specific bit positions, not the
  // cross-chip XPOWERS_PWR_BTN_* enums (which would write the wrong
  // INTEN register).
  s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
  s_pmu.clearIrqStatus();

  // First snapshot. settingsLoad() has not run yet, so this only fills the
  // cache with sane values; the governor takes over on the first bus tick.
  hwPowerTick(0);
  return true;
}

void hwPowerTick(uint32_t nowMs) {
  uint32_t dt = s_tickInit ? (nowMs - s_lastTickMs) : 1000u;
  s_tickInit   = true;
  s_lastTickMs = nowMs;
  if (dt < 200u) dt = 200u;
  if (dt > 60000u) dt = 60000u;

  const int   mv       = (int)s_pmu.getBattVoltage();
  const int   vbus     = (int)s_pmu.getVbusVoltage();
  const bool  usb      = vbus > 4000;
  const bool  charging = s_pmu.isCharging();
  const uint8_t chgSt  = (uint8_t)s_pmu.getChargerStatus();

  int gauge = s_pmu.getBatteryPercent();
  if (gauge < 0 || gauge > 100) gauge = -1;

  const float tempF = s_pmu.getTemperature();
  const bool  tempOk = battTempValid(tempF);
  const int   tempC  = tempOk ? (int)lroundf(tempF) : 0;

  const NovaSettings& cfg = settingsGet();
  const uint8_t limit = (cfg.chargeLimitPct >= kBattLimitMin &&
                         cfg.chargeLimitPct <= kBattLimitMax)
                          ? cfg.chargeLimitPct
                          : 90;

  // Governor sees the previous estimate; the estimator sees the new current.
  const ChargeDecision dec = chargeGovernorStep(
      s_gov, cfg.chargeSpeed, s_screenOn, tempF, s_model.pct, limit, usb, nowMs);
  applyCharging(dec.enable, dec.ma);

  BatteryIn in;
  in.mV       = (uint16_t)mv;
  in.usb      = usb;
  in.charging = charging;
  in.chgState = chgSt;
  in.gaugePct = gauge;
  in.tempC    = tempF;
  in.dtMs     = dt;
  in.targetMa = dec.enable ? dec.ma : s_appliedMa;
  in.limited  = dec.limited;
  in.limitPct = limit;

  const BatteryOut out = batteryModelStep(s_model, in);

  portENTER_CRITICAL(&s_mux);
  s_bat.mV            = (mv >= 0) ? mv : 0;
  s_bat.mA            = 0;
  s_bat.pct           = out.pct;
  s_bat.usbPresent    = usb;
  s_bat.charging      = charging;
  s_bat.tempC         = tempOk ? tempC : s_bat.tempC;
  s_bat.stage         = (uint8_t)out.stage;
  s_bat.chgMa         = dec.enable ? dec.ma : 0;
  s_bat.chargeLimited = dec.limited;
  s_bat.gaugePct      = (int8_t)gauge;
  portEXIT_CRITICAL(&s_mux);
}

void hwPowerSetScreenOn(bool on) { s_screenOn = on; }

HwBattery hwBattery() {
  HwBattery b;
  portENTER_CRITICAL(&s_mux);
  b = s_bat;
  portEXIT_CRITICAL(&s_mux);
  return b;
}

void hwPowerOff() { s_pmu.shutdown(); }

bool hwAxpPekeyShortPress() {
  s_pmu.getIrqStatus();
  bool hit = s_pmu.isPekeyShortPressIrq();
  if (hit) s_pmu.clearIrqStatus();
  return hit;
}

bool hwAxpPekeyLongPress() {
  s_pmu.getIrqStatus();
  bool hit = s_pmu.isPekeyLongPressIrq();
  if (hit) s_pmu.clearIrqStatus();
  return hit;
}

XPowersPMU* hwPmuRef() { return &s_pmu; }
