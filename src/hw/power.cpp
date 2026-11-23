#include "hw/power.h"
#include "hw/pins.h"
#include "model/settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <math.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>

namespace {

XPowersPMU s_pmu;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
HwBattery      s_bat = {};
PowerWatermark s_wm = {};
PowerWatermark s_wmSnap = {};
volatile bool  s_aodActive = false;
uint8_t        s_pwrOnSrc = 0;
uint8_t        s_pwrOffSrc = 0;

constexpr size_t kPowerEvtCap = 12;
PowerEvent       s_evtRing[kPowerEvtCap] = {};
size_t           s_evtHead = 0;
size_t           s_evtTail = 0;
size_t           s_evtCount = 0;
uint32_t         s_evtSeq = 0;

bool     s_evtInited       = false;
bool     s_prevEvtUsb      = false;
uint8_t  s_prevEvtStage    = 0;
bool     s_prevEvtThermReg = false;
bool     s_prevEvtTempCap  = false;
uint32_t s_lastThermEvtMs  = 0;
uint8_t  s_lowBatSentMask  = 0;
uint32_t s_prevWmTickMs    = 0;
uint16_t s_lastRestMv      = 0;

void pushPowerEvent(PowerEvent ev) {
  portENTER_CRITICAL(&s_mux);
  ev.seq = ++s_evtSeq;
  if (s_evtCount >= kPowerEvtCap) {
    // Keep the initial BOOT blackbox event intact if MQTT hasn't drained it yet.
    if (s_evtRing[s_evtHead].reason == BattPwrEvtReason::BOOT) {
      const size_t next = (s_evtHead + 1u) % kPowerEvtCap;
      s_evtRing[next] = s_evtRing[s_evtHead];
    }
    s_evtHead = (s_evtHead + 1u) % kPowerEvtCap;
    s_evtCount--;
  }
  s_evtRing[s_evtTail] = ev;
  s_evtTail = (s_evtTail + 1u) % kPowerEvtCap;
  s_evtCount++;
  portEXIT_CRITICAL(&s_mux);
}

BatteryModel    s_model;
ChargeGovernor  s_gov;
BatteryCal      s_cal;
uint32_t        s_lastTickMs = 0;
bool            s_tickInit   = false;
volatile bool   s_screenOn   = true;
volatile bool   s_resetCalReq = false;

// Boot-time unloaded OCV + PMU/NVS continuity anchors.
uint16_t        s_bootRestMv        = 0;
bool            s_hasBootRestMv     = false;
int16_t         s_pmuSeedPct        = -1;
bool            s_pmuSeedLimitCut   = false;
bool            s_pmuPowerLost      = true;
bool            s_bootSeedApplied   = false;
int16_t         s_nvsSavedPct       = -1;
uint32_t        s_lastNvsSaveMs     = 0;
int16_t         s_pmuLastWrittenPct = -1;
bool            s_pmuLastWrittenCut = false;

// Self-calibration runtime trackers (all on Core 0 inside hwPowerTick).
constexpr uint32_t kProbeFirstDelayMs = 30000u;
constexpr uint32_t kProbePeriodMs     = 720000u; // 12 min
bool            s_probePending      = false;
uint16_t        s_probePreMv        = 0;
uint16_t        s_probeMa           = 0;
bool            s_probeVbusLim      = false;
uint32_t        s_ccSinceProbeMs    = 0;
bool            s_hadFirstCcProbe   = false;

bool            s_spanActive        = false;
uint16_t        s_spanStartOcvMv    = 0;
uint64_t        s_spanMaMs          = 0;

bool            s_inCv              = false;
uint32_t        s_cvDurMs           = 0;
uint32_t        s_cvPinned99Ms      = 0;
uint8_t         s_cvEndPct          = 0;

uint32_t        s_doneRestMs        = 0;
bool            s_doneRestRecorded  = false;

// Applied charge config, so the governor only touches I2C when it changes.
bool     s_appliedEnable = true;
uint16_t s_appliedMa     = 300;
uint8_t  s_appliedCurr   = 0xFF;

constexpr uint8_t kPmuBufMagic = 0xA5;
constexpr uint8_t kPmuBufXor   = 0x5A;

void readPmuAnchor() {
  uint8_t buf[4] = {};
  // Note: XPowersLib's readDataBuffer() casts readRegister()'s 0-on-success int
  // return directly to bool (returning false on success!), so call readRegister().
  if (s_pmu.readRegister(XPOWERS_AXP2101_DATA_BUFFER1, buf, sizeof(buf)) == 0 &&
      buf[0] == kPmuBufMagic && buf[1] <= 100 &&
      buf[3] == (uint8_t)(buf[0] ^ buf[1] ^ buf[2] ^ kPmuBufXor)) {
    s_pmuSeedPct        = (int16_t)buf[1];
    s_pmuSeedLimitCut   = (buf[2] & 0x01) != 0;
    s_pmuPowerLost      = false;
    s_pmuLastWrittenPct = s_pmuSeedPct;
    s_pmuLastWrittenCut = s_pmuSeedLimitCut;
  } else {
    s_pmuSeedPct        = -1;
    s_pmuSeedLimitCut   = false;
    s_pmuPowerLost      = true;
    s_pmuLastWrittenPct = -1;
    s_pmuLastWrittenCut = false;
  }
}

void writePmuAnchor(uint8_t pct, bool limitCut) {
  if ((int16_t)pct == s_pmuLastWrittenPct && limitCut == s_pmuLastWrittenCut) return;
  uint8_t buf[4] = {
    kPmuBufMagic,
    pct,
    (uint8_t)(limitCut ? 0x01 : 0x00),
    0,
  };
  buf[3] = (uint8_t)(buf[0] ^ buf[1] ^ buf[2] ^ kPmuBufXor);
  if (s_pmu.writeRegister(XPOWERS_AXP2101_DATA_BUFFER1, buf, sizeof(buf)) == 0) {
    s_pmuLastWrittenPct = (int16_t)pct;
    s_pmuLastWrittenCut = limitCut;
  }
}

int16_t loadNvsBatteryPct() {
  Preferences prefs;
  int16_t pct = -1;
  if (prefs.begin("nova_bat", true)) {
    const int32_t v = prefs.getShort("pct", -1);
    if (v >= 0 && v <= 100) pct = (int16_t)v;
    prefs.end();
  }
  return pct;
}

void saveNvsBatteryPct(uint8_t pct) {
  Preferences prefs;
  if (prefs.begin("nova_bat", false)) {
    prefs.putShort("pct", (int16_t)pct);
    prefs.end();
    s_nvsSavedPct = (int16_t)pct;
  }
}

void loadNvsBatteryCal(BatteryCal& cal) {
  cal = BatteryCal{};
  Preferences prefs;
  if (prefs.begin("nova_bat", true)) {
    if (prefs.getUShort("cal_ver", 0) == 1) {
      cal.chgMohm     = prefs.getUShort("r_chg", cal.chgMohm);
      cal.designMah   = prefs.getUShort("mah", cal.designMah);
      cal.cvTauMs     = prefs.getULong("cv_tau", cal.cvTauMs);
      cal.fullRestMv  = prefs.getUShort("full_mv", cal.fullRestMv);
      cal.lowAnchorMv = prefs.getUShort("low_mv", cal.lowAnchorMv);
      cal.rSamples    = prefs.getUShort("r_cnt", 0);
      cal.capSamples  = prefs.getUShort("c_cnt", 0);
      cal.cvSamples   = prefs.getUShort("cv_cnt", 0);
      battCalSanitize(cal);
    }
    prefs.end();
  }
}

void saveNvsBatteryCal(const BatteryCal& cal) {
  Preferences prefs;
  if (prefs.begin("nova_bat", false)) {
    prefs.putUShort("cal_ver", 1);
    prefs.putUShort("r_chg", cal.chgMohm);
    prefs.putUShort("mah", cal.designMah);
    prefs.putULong("cv_tau", cal.cvTauMs);
    prefs.putUShort("full_mv", cal.fullRestMv);
    prefs.putUShort("low_mv", cal.lowAnchorMv);
    prefs.putUShort("r_cnt", cal.rSamples);
    prefs.putUShort("c_cnt", cal.capSamples);
    prefs.putUShort("cv_cnt", cal.cvSamples);
    prefs.end();
  }
}

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

  // Read PMU battery-backed RAM anchor before modifying charge state: if valid,
  // PMU kept power across reboot; if wiped, PMU suffered a power-loss (e.g.
  // overnight empty cutoff).
  readPmuAnchor();
  {
    const int onRaw  = (int)s_pmu.getPowerOnSource();
    const int offRaw = (int)s_pmu.getPowerOffSource();
    s_pwrOnSrc  = (onRaw >= 0) ? (uint8_t)onRaw : 0;
    s_pwrOffSrc = (offRaw >= 0) ? (uint8_t)offRaw : 0;
    Serial.printf("[bat] blackbox on=0x%02X off=0x%02X pmuLost=%d seed=%d\n",
                  s_pwrOnSrc, s_pwrOffSrc, s_pmuPowerLost ? 1 : 0, (int)s_pmuSeedPct);
  }

  // Let the newly-enabled ADC complete at least one conversion, then capture an
  // unloaded resting OCV sample on USB before enabling 300mA charging.
  delay(40);
  const bool usbNow  = (s_pmu.getVbusVoltage() > 4000) || s_pmu.isVbusIn();
  const bool batConn = s_pmu.isBatteryConnect();
  const uint16_t probeMv = batConn ? s_pmu.getBattVoltage() : 0;
  s_bootRestMv    = 0;
  s_hasBootRestMv = false;
  if (usbNow && batConn && probeMv >= 2800) {
    // VSYS stays powered by VBUS; wait 220ms with charging disabled so both
    // ohmic I*R drop and AXP ADC conversion settle cleanly.
    s_pmu.disableCellbatteryCharge();
    delay(200);
    const uint16_t r1 = s_pmu.getBattVoltage();
    delay(25);
    const uint16_t r2 = s_pmu.getBattVoltage();
    const uint16_t restMv = battMvValid(r2) ? r2 : r1;
    if (battMvValid(restMv)) {
      s_bootRestMv    = restMv;
      s_hasBootRestMv = true;
    }
  } else if (battMvValid(probeMv)) {
    s_bootRestMv    = probeMv;
    s_hasBootRestMv = !usbNow;
  }

  // The charge-enable bit survives a reset. If the previous run paused at the
  // cap, the pack would stay uncharged forever; re-arm explicitly on boot
  // (governor will immediately re-apply cap/thermal pause after settingsLoad()).
  s_pmu.enableCellbatteryCharge();
  if (usbNow) delay(20);
  s_appliedEnable = true;
  s_appliedMa     = 300;

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

  // Do NOT run hwPowerTick(0) here: settingsLoad() has not loaded chargeLimitPct
  // yet. setup() invokes hwPowerTick(millis()) right after settingsLoad().
  return true;
}

void hwPowerTick(uint32_t nowMs) {
  if (s_tickInit && (uint32_t)(nowMs - s_lastTickMs) < 200u) return;
  uint32_t dt = s_tickInit ? (uint32_t)(nowMs - s_lastTickMs) : 1000u;
  s_tickInit   = true;
  s_lastTickMs = nowMs;
  if (dt < 200u) dt = 200u;
  if (dt > 60000u) dt = 60000u;

  int     mv       = (int)s_pmu.getBattVoltage();
  const int vbus   = (int)s_pmu.getVbusVoltage();
  const int vsys   = (int)s_pmu.getSystemVoltage();
  const bool usb   = vbus > 4000;
  bool    charging = s_pmu.isCharging();
  uint8_t chgSt    = (uint8_t)s_pmu.getChargerStatus();
  const bool vbusLim  = usb && s_pmu.getCurrentLimitStatus();
  const bool thermReg = s_pmu.getThermalRegulationStatus();

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

  bool calDirty = false;
  if (s_resetCalReq) {
    s_resetCalReq = false;
    s_cal = BatteryCal{};
    s_spanActive = false;
    s_spanMaMs = 0;
    s_hadFirstCcProbe = false;
    s_ccSinceProbeMs = 0;
    calDirty = true;
  }

  int16_t seedPct = -1;
  bool    seedLost = s_pmuPowerLost;
  if (!s_bootSeedApplied) {
    s_bootSeedApplied = true;
    s_nvsSavedPct = loadNvsBatteryPct();
    loadNvsBatteryCal(s_cal);
    if (!s_pmuPowerLost && s_pmuSeedPct >= 0) {
      seedPct = s_pmuSeedPct;
      if (usb && s_pmuSeedLimitCut && limit < 100) {
        s_gov.limitCut = true;
      }
    } else {
      seedPct = s_nvsSavedPct;
      seedLost = true;
      if (battCalOnUnexpectedCutoff(s_cal, seedPct, true)) {
        calDirty = true;
      }
    }
  } else {
    seedPct = !s_pmuPowerLost ? s_pmuSeedPct : s_nvsSavedPct;
  }

  // Complete a 1s non-blocking unloaded OCV probe started on the previous tick.
  uint16_t probeOcvMv = 0;
  if (s_probePending) {
    s_probePending = false;
    const uint16_t vRest = (uint16_t)mv;
    s_pmu.enableCellbatteryCharge();
    s_appliedEnable = true;
    if (usb && battMvValid(vRest)) {
      probeOcvMv = vRest;
      if (!s_probeVbusLim &&
          battCalOnChargePause(s_cal, s_probePreMv, vRest, s_probeMa)) {
        calDirty = true;
      }
      if (!s_spanActive) {
        if (vRest >= 3520u && vRest <= 3980u) {
          s_spanActive = true;
          s_spanStartOcvMv = vRest;
          s_spanMaMs = 0;
        }
      } else {
        const uint32_t chargedUah = (uint32_t)(s_spanMaMs / 3600u);
        if (battCalOnCapacitySpan(s_cal, s_spanStartOcvMv, vRest, chargedUah)) {
          calDirty = true;
          s_spanStartOcvMv = vRest;
          s_spanMaMs = 0;
        }
      }
      // Keep the pre-probe terminal voltage and CC stage for this tick so the
      // UI and smoothMv never flicker during the 1s measurement pause.
      mv = (int)s_probePreMv;
      charging = true;
      chgSt = AXP_CHG_CC;
    } else {
      s_spanActive = false;
      s_spanMaMs = 0;
    }
  }

  uint8_t govPct = s_model.pct;
  if (!s_model.init) {
    if (!seedLost && seedPct >= 0 && seedPct <= 100) {
      govPct = (uint8_t)seedPct;
    } else if (s_hasBootRestMv && battMvValid(s_bootRestMv)) {
      govPct = battOcvPct((int)s_bootRestMv);
    } else if (battMvValid((uint16_t)mv)) {
      govPct = battOcvPct(mv);
    }
  }

  // Governor sees the previous (or boot-seeded) estimate; estimator sees new current.
  const ChargeDecision dec = chargeGovernorStep(
      s_gov, cfg.chargeSpeed, s_screenOn, tempF, govPct, limit, usb, nowMs);
  applyCharging(dec.enable, dec.ma);

  BatteryIn in;
  in.mV             = battMvValid((uint16_t)mv) ? (uint16_t)mv
                                                : (s_hasBootRestMv ? s_bootRestMv : 0);
  in.usb            = usb;
  in.charging       = charging;
  in.chgState       = chgSt;
  in.gaugePct       = gauge;
  in.tempC          = tempF;
  in.dtMs           = dt;
  in.targetMa       = dec.enable ? dec.ma : s_appliedMa;
  in.limited        = dec.limited;
  in.limitPct       = limit;
  in.screenOn       = s_screenOn;
  in.seedPct        = seedPct;
  in.seedPowerLost  = seedLost;
  in.seedMvValid    = !s_model.init && s_hasBootRestMv && battMvValid(s_bootRestMv);
  in.seedMv         = s_bootRestMv;
  in.calChgMohm     = s_cal.chgMohm;
  in.calDesignMah   = s_cal.designMah;
  in.calCvTauMs     = s_cal.cvTauMs;
  in.calLowAnchorMv = s_cal.lowAnchorMv;
  in.probeOcvMv     = probeOcvMv;

  const BatteryOut out = batteryModelStep(s_model, in);

  // Track CC span & periodic 1s OCV probe schedule.
  if (out.stage == BattStage::CC && dec.enable && !vbusLim && probeOcvMv == 0) {
    s_ccSinceProbeMs += dt;
    if (s_spanActive) {
      s_spanMaMs += (uint64_t)out.effMa * dt;
    }
    const uint32_t probeInterval = s_hadFirstCcProbe ? kProbePeriodMs : kProbeFirstDelayMs;
    if (s_ccSinceProbeMs >= probeInterval && mv < 4175 && out.effMa >= 150u) {
      s_ccSinceProbeMs  = 0;
      s_hadFirstCcProbe = true;
      s_probePending    = true;
      s_probePreMv      = (uint16_t)mv;
      s_probeMa         = out.effMa;
      s_probeVbusLim    = vbusLim;
      s_pmu.disableCellbatteryCharge();
      s_appliedEnable   = false;
    }
  } else if (out.stage != BattStage::CC) {
    s_ccSinceProbeMs = 0;
    s_hadFirstCcProbe = false;
    if (out.stage != BattStage::CV) {
      s_spanActive = false;
      s_spanMaMs = 0;
    }
  } else if (vbusLim) {
    s_spanActive = false;
    s_spanMaMs = 0;
  }

  // Track CV-to-DONE duration for online CV taper calibration.
  if (out.stage == BattStage::CV) {
    s_inCv = true;
    s_cvDurMs += dt;
    s_cvEndPct = out.pct;
    if (out.pct >= 99u) s_cvPinned99Ms += dt;
  } else if (out.stage == BattStage::DONE) {
    if (s_inCv) {
      s_inCv = false;
      if (battCalOnChargeDone(s_cal, s_cvEndPct, s_cvDurMs, s_cvPinned99Ms)) {
        calDirty = true;
      }
      s_cvDurMs = 0;
      s_cvPinned99Ms = 0;
    }
    if (!s_doneRestRecorded && !charging) {
      s_doneRestMs += dt;
      if (s_doneRestMs >= 60000u && battCalOnFullRest(s_cal, (uint16_t)mv)) {
        s_doneRestRecorded = true;
        calDirty = true;
      }
    }
  } else {
    s_inCv = false;
    s_cvDurMs = 0;
    s_cvPinned99Ms = 0;
    s_doneRestMs = 0;
    s_doneRestRecorded = false;
  }

  if (calDirty) {
    saveNvsBatteryCal(s_cal);
  }

  if (s_model.init && out.stage != BattStage::NONE) {
    writePmuAnchor(out.pct, dec.limitCut);
    const int nvsDiff = (s_nvsSavedPct >= 0) ? abs((int)out.pct - (int)s_nvsSavedPct) : 100;
    const bool needLowSave  = (out.pct <= 10 && nvsDiff >= 2);
    const bool needStepSave = (nvsDiff >= 5 && (uint32_t)(nowMs - s_lastNvsSaveMs) >= 60000u);
    if (s_nvsSavedPct < 0 || needLowSave || needStepSave) {
      saveNvsBatteryPct(out.pct);
      s_lastNvsSaveMs = nowMs;
    }
  }

  const uint16_t cleanBatMv  = battMvValid((uint16_t)mv) ? (uint16_t)mv : 0;
  const uint16_t cleanVbusMv = (vbus > 0 && vbus < 20000) ? (uint16_t)vbus : 0;
  const uint16_t cleanVsysMv = (vsys > 0 && vsys < 20000) ? (uint16_t)vsys : 0;
  const int16_t  wmTempC     = tempOk ? (int16_t)tempC : (int16_t)-128;

  auto buildPwrEvt = [&](BattPwrEvtReason reason, bool includeWm) -> PowerEvent {
    PowerEvent ev{};
    ev.reason        = reason;
    ev.pct           = out.pct;
    ev.rawPct        = out.rawPct;
    ev.gaugePct      = (int8_t)gauge;
    ev.seedPct       = (reason == BattPwrEvtReason::BOOT && seedPct >= 0 && seedPct <= 100)
                         ? (int8_t)seedPct : -1;
    ev.stage         = (uint8_t)out.stage;
    ev.prevStage     = s_prevEvtStage;
    ev.usbPresent    = usb;
    ev.charging      = charging;
    ev.vbusLimited   = vbusLim;
    ev.thermReg      = thermReg;
    ev.tempCapped    = dec.tempCapped;
    ev.limited       = dec.limited;
    ev.seedPowerLost = seedLost;
    ev.prevUsb       = s_prevEvtUsb;
    ev.mv            = cleanBatMv;
    ev.compMv        = out.compMv;
    ev.restMv        = (reason == BattPwrEvtReason::PROBE && probeOcvMv > 0)
                         ? probeOcvMv
                         : ((reason == BattPwrEvtReason::BOOT) ? s_bootRestMv : s_lastRestMv);
    ev.vbusMv        = cleanVbusMv;
    ev.vsysMv        = cleanVsysMv;
    ev.chgMa         = (out.stage == BattStage::DONE) ? 0 : (dec.enable ? dec.ma : 0);
    ev.effMa         = out.effMa;
    ev.tempC         = (int16_t)(tempOk ? tempC : s_bat.tempC);
    ev.pwrOnSrc      = s_pwrOnSrc;
    ev.pwrOffSrc     = s_pwrOffSrc;
    ev.chgMohm       = s_cal.chgMohm;
    ev.designMah     = s_cal.designMah;
    if (includeWm) powerWatermarkAttach(s_wm, ev);
    return ev;
  };

  if (!s_evtInited) {
    s_evtInited       = true;
    s_prevEvtUsb      = usb;
    s_prevEvtStage    = (uint8_t)out.stage;
    s_prevEvtThermReg = thermReg;
    s_prevEvtTempCap  = dec.tempCapped;
    s_prevWmTickMs    = nowMs;
    s_lastRestMv      = s_bootRestMv;
    powerWatermarkReset(s_wm, usb, cleanBatMv, cleanVsysMv, cleanVbusMv, wmTempC, out.pct);
    pushPowerEvent(buildPwrEvt(BattPwrEvtReason::BOOT, false));
  } else {
    const uint32_t wmDt = (nowMs >= s_prevWmTickMs) ? (nowMs - s_prevWmTickMs) : 0;
    s_prevWmTickMs = nowMs;
    powerWatermarkAccum(s_wm, wmDt, usb, cleanBatMv, cleanVsysMv, cleanVbusMv, wmTempC,
                        vbusLim, thermReg, dec.tempCapped, s_screenOn, s_aodActive);

    if (probeOcvMv > 0) {
      s_lastRestMv = probeOcvMv;
      pushPowerEvent(buildPwrEvt(BattPwrEvtReason::PROBE, true));
    }

    const bool usbChanged   = (usb != s_prevEvtUsb);
    const bool stageChanged = ((uint8_t)out.stage != s_prevEvtStage);
    const bool thermChanged = (thermReg != s_prevEvtThermReg) || (dec.tempCapped != s_prevEvtTempCap);

    if (usbChanged) {
      pushPowerEvent(buildPwrEvt(usb ? BattPwrEvtReason::USB_IN : BattPwrEvtReason::USB_OUT, true));
      powerWatermarkReset(s_wm, usb, cleanBatMv, cleanVsysMv, cleanVbusMv, wmTempC, out.pct);
      s_prevEvtThermReg = thermReg;
      s_prevEvtTempCap  = dec.tempCapped;
      if (usb) s_lowBatSentMask = 0;
    } else if (stageChanged) {
      pushPowerEvent(buildPwrEvt(BattPwrEvtReason::STAGE, true));
      s_prevEvtThermReg = thermReg;
      s_prevEvtTempCap  = dec.tempCapped;
    } else if (thermChanged &&
               (s_lastThermEvtMs == 0 || (uint32_t)(nowMs - s_lastThermEvtMs) >= 10000u)) {
      s_lastThermEvtMs  = nowMs;
      s_prevEvtThermReg = thermReg;
      s_prevEvtTempCap  = dec.tempCapped;
      pushPowerEvent(buildPwrEvt(BattPwrEvtReason::THERM, true));
    }

    if (usb) {
      s_lowBatSentMask = 0;
    } else {
      if (out.pct >= 19u) s_lowBatSentMask &= (uint8_t)~0x01u;
      if (out.pct >= 9u)  s_lowBatSentMask &= (uint8_t)~0x02u;
      if (out.pct <= 5u && (s_lowBatSentMask & 0x02u) == 0) {
        s_lowBatSentMask |= 0x03u;
        pushPowerEvent(buildPwrEvt(BattPwrEvtReason::LOW_BAT, true));
      } else if (out.pct <= 15u && (s_lowBatSentMask & 0x01u) == 0) {
        s_lowBatSentMask |= 0x01u;
        pushPowerEvent(buildPwrEvt(BattPwrEvtReason::LOW_BAT, true));
      }
    }

    s_prevEvtUsb   = usb;
    s_prevEvtStage = (uint8_t)out.stage;
  }

  portENTER_CRITICAL(&s_mux);
  s_bat.mV               = (mv >= 0) ? mv : 0;
  s_bat.mA               = 0;
  s_bat.pct              = out.pct;
  s_bat.usbPresent       = usb;
  s_bat.charging         = charging;
  s_bat.tempC            = tempOk ? tempC : s_bat.tempC;
  s_bat.stage            = (uint8_t)out.stage;
  // A terminated charge is not "charging at 300mA" even though the CC register
  // still holds 300 — hide the number once the PMU says DONE.
  s_bat.chgMa            = (out.stage == BattStage::DONE) ? 0 : (dec.enable ? dec.ma : 0);
  s_bat.chargeLimited    = dec.limited;
  s_bat.chargeTempCapped = dec.tempCapped;
  s_bat.gaugePct         = (int8_t)gauge;
  s_bat.rawPct           = out.rawPct;
  s_bat.compMv           = out.compMv;
  s_bat.effMa            = out.effMa;
  s_bat.bootMv           = s_bootRestMv;
  s_bat.seedPct          = (seedPct >= 0 && seedPct <= 100) ? (int8_t)seedPct : -1;
  s_bat.seedPowerLost    = seedLost;
  s_bat.vbusLimited      = vbusLim;
  s_bat.vbusMv           = cleanVbusMv;
  s_bat.vsysMv           = cleanVsysMv;
  s_bat.thermReg         = thermReg;
  s_bat.pwrOnSrc         = s_pwrOnSrc;
  s_bat.pwrOffSrc        = s_pwrOffSrc;
  s_wmSnap               = s_wm;
  portEXIT_CRITICAL(&s_mux);
}

void hwPowerSetScreenOn(bool on) { s_screenOn = on; }
void hwPowerSetAodActive(bool active) { s_aodActive = active; }

HwBattery hwBattery() {
  HwBattery b;
  portENTER_CRITICAL(&s_mux);
  b = s_bat;
  portEXIT_CRITICAL(&s_mux);
  return b;
}

BatteryCal hwBatteryCal() {
  BatteryCal c;
  portENTER_CRITICAL(&s_mux);
  c = s_cal;
  portEXIT_CRITICAL(&s_mux);
  return c;
}

PowerWatermark hwPowerWatermark() {
  PowerWatermark w;
  portENTER_CRITICAL(&s_mux);
  w = s_wmSnap;
  portEXIT_CRITICAL(&s_mux);
  return w;
}

bool hwPowerPeekEvent(PowerEvent* out) {
  if (!out) return false;
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s_evtCount > 0) {
    *out = s_evtRing[s_evtHead];
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void hwPowerAckEvent() {
  portENTER_CRITICAL(&s_mux);
  if (s_evtCount > 0) {
    s_evtHead = (s_evtHead + 1u) % kPowerEvtCap;
    s_evtCount--;
  }
  portEXIT_CRITICAL(&s_mux);
}

void hwPowerResetCal() { s_resetCalReq = true; }

void hwPowerOff() {
  if (s_model.init) {
    writePmuAnchor(s_model.pct, s_gov.limitCut);
    saveNvsBatteryPct(s_model.pct);
  }
  s_pmu.shutdown();
}

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
