#pragma once
// Pure battery estimation / charge-policy logic.
//
// Kept free of Arduino and I2C so the same code runs in the host unit test
// (tools/test_battery_model.cpp) and in the firmware. State is passed in by
// reference — no file-scope statics, no per-TU copies.
//
// Why this exists: hwBattery() used to do `pct = (mV - 3200) / 10`. A LiPo is
// not linear, and under charge the terminal voltage is OCV + I*R, so that
// formula jumped 40% -> 90% in minutes and hit 0% while the PMU was still
// happily running at 3.2V. This model separates the two questions:
//   - "how much is left"  -> OCV table + stage machine + anchors + slew limit
//   - "how fast to charge" -> chargeGovernorStep() policy table
// AXP2101 gives no battery current on this board, so no coulomb counting.

#include <math.h>
#include <stdint.h>

// Keyframe reason codes for bounded device-side power event telemetry (`evt:"pwr"`).
enum class BattPwrEvtReason : uint8_t {
  NONE    = 0,
  BOOT    = 1,  // cold/warm boot blackbox (PWRON/PWROFF_STATUS, bootMv, seed)
  USB_IN  = 2,  // VBUS plugged in (closes previous discharge session watermark)
  USB_OUT = 3,  // VBUS unplugged (closes previous charge session watermark)
  STAGE   = 4,  // charge/discharge stage transition (PRE->CC->CV->DONE/LIMITED)
  PROBE   = 5,  // periodic unloaded OCV probe completed
  THERM   = 6,  // thermal regulation / temp-cap entry or recovery
  LOW_BAT = 7,  // battery SOC crossed low-battery threshold
};

inline const char* battPwrEvtReasonStr(BattPwrEvtReason r) {
  switch (r) {
    case BattPwrEvtReason::BOOT:    return "boot";
    case BattPwrEvtReason::USB_IN:  return "usb_in";
    case BattPwrEvtReason::USB_OUT: return "usb_out";
    case BattPwrEvtReason::STAGE:   return "stage";
    case BattPwrEvtReason::PROBE:   return "probe";
    case BattPwrEvtReason::THERM:   return "therm";
    case BattPwrEvtReason::LOW_BAT: return "low_bat";
    default:                        return "none";
  }
}

// Per-session in-RAM min/max watermarks and timers (never stored to flash loop-wise).
struct PowerWatermark {
  bool     active       = false;
  bool     chargingSess = false;
  uint32_t durMs        = 0;
  uint32_t vbusLimMs    = 0;
  uint32_t thermRegMs   = 0;
  uint32_t tempCapMs    = 0;
  uint32_t screenOnMs   = 0;
  uint32_t aodMs        = 0;
  uint16_t vbatMinMv    = 0;
  uint16_t vbatMaxMv    = 0;
  uint16_t vsysMinMv    = 0;
  uint16_t vbusMinMv    = 0;
  int16_t  tmpMaxC      = -128;
  uint8_t  startPct     = 0;
};

struct PowerEvent {
  BattPwrEvtReason reason = BattPwrEvtReason::NONE;
  uint8_t  pct           = 0;
  uint8_t  rawPct        = 0;
  int8_t   gaugePct      = -1;
  int8_t   seedPct       = -1;
  uint8_t  stage         = 0;
  uint8_t  prevStage     = 0;
  bool     usbPresent    = false;
  bool     charging      = false;
  bool     vbusLimited   = false;
  bool     thermReg      = false;
  bool     tempCapped    = false;
  bool     limited       = false;
  bool     seedPowerLost = false;
  bool     prevUsb       = false;
  uint16_t mv            = 0;
  uint16_t compMv        = 0;
  uint16_t restMv        = 0;      // boot unloaded mV or latest OCV probe mV
  uint16_t vbusMv        = 0;
  uint16_t vsysMv        = 0;
  uint16_t chgMa         = 0;
  uint16_t effMa         = 0;
  int16_t  tempC         = 0;
  uint8_t  pwrOnSrc      = 0;      // AXP2101 PWRON_STATUS (0x20)
  uint8_t  pwrOffSrc     = 0;      // AXP2101 PWROFF_STATUS (0x21)
  uint16_t chgMohm       = 0;      // BatteryCal::chgPackMohm
  uint16_t designMah     = 0;      // BatteryCal::designCapMah
  uint32_t seq           = 0;      // monotonic event counter
  // Attached session watermark snapshot
  bool     hasWatermark  = false;
  uint32_t durSec        = 0;
  uint16_t vbatMinMv     = 0;
  uint16_t vbatMaxMv     = 0;
  uint16_t vsysMinMv     = 0;
  uint16_t vbusMinMv     = 0;
  int16_t  tmpMaxC       = -128;
  uint16_t vbusLimSec    = 0;
  uint16_t thermRegSec   = 0;
  uint16_t tempCapSec    = 0;
  uint16_t screenOnSec   = 0;
  uint16_t aodSec        = 0;
  uint8_t  startPct      = 0;
};

inline void powerWatermarkReset(PowerWatermark& wm,
                                bool chargingSess,
                                uint16_t vbatMv,
                                uint16_t vsysMv,
                                uint16_t vbusMv,
                                int16_t tempC,
                                uint8_t pct) {
  wm = PowerWatermark{};
  wm.active       = true;
  wm.chargingSess = chargingSess;
  wm.startPct     = pct;
  wm.vbatMinMv    = vbatMv;
  wm.vbatMaxMv    = vbatMv;
  wm.vsysMinMv    = vsysMv;
  wm.vbusMinMv    = (chargingSess && vbusMv > 0) ? vbusMv : 0;
  wm.tmpMaxC      = (tempC > -40 && tempC < 125) ? tempC : -128;
}

inline void powerWatermarkAccum(PowerWatermark& wm,
                                uint32_t dtMs,
                                bool usbPresent,
                                uint16_t vbatMv,
                                uint16_t vsysMv,
                                uint16_t vbusMv,
                                int16_t tempC,
                                bool vbusLim,
                                bool thermReg,
                                bool tempCap,
                                bool screenOn,
                                bool aodActive) {
  if (!wm.active) {
    powerWatermarkReset(wm, usbPresent, vbatMv, vsysMv, vbusMv, tempC, 0);
  }
  if (dtMs > 10000u) dtMs = 10000u;
  wm.durMs += dtMs;
  if (vbusLim)  wm.vbusLimMs  += dtMs;
  if (thermReg) wm.thermRegMs += dtMs;
  if (tempCap)  wm.tempCapMs  += dtMs;
  // AOD keeps the panel powered at reduced brightness; keep AOD time separate
  // from active screen time so discharge budgets can distinguish them.
  if (aodActive) {
    wm.aodMs += dtMs;
  } else if (screenOn) {
    wm.screenOnMs += dtMs;
  }
  if (vbatMv > 0) {
    if (wm.vbatMinMv == 0 || vbatMv < wm.vbatMinMv) wm.vbatMinMv = vbatMv;
    if (vbatMv > wm.vbatMaxMv) wm.vbatMaxMv = vbatMv;
  }
  if (vsysMv > 0 && (wm.vsysMinMv == 0 || vsysMv < wm.vsysMinMv)) {
    wm.vsysMinMv = vsysMv;
  }
  if (usbPresent && vbusMv > 0 && (wm.vbusMinMv == 0 || vbusMv < wm.vbusMinMv)) {
    wm.vbusMinMv = vbusMv;
  }
  if (tempC > -40 && tempC < 125 && tempC > wm.tmpMaxC) {
    wm.tmpMaxC = tempC;
  }
}

inline uint16_t powerMsToClampedSec(uint32_t ms) {
  const uint32_t sec = (ms + 500u) / 1000u;
  return (sec > 65535u) ? 65535u : (uint16_t)sec;
}

inline void powerWatermarkAttach(const PowerWatermark& wm, PowerEvent& ev) {
  ev.hasWatermark = wm.active;
  if (!wm.active) return;
  ev.durSec      = (wm.durMs + 500u) / 1000u;
  ev.vbatMinMv   = wm.vbatMinMv;
  ev.vbatMaxMv   = wm.vbatMaxMv;
  ev.vsysMinMv   = wm.vsysMinMv;
  ev.vbusMinMv   = wm.vbusMinMv;
  ev.tmpMaxC     = wm.tmpMaxC;
  ev.vbusLimSec  = powerMsToClampedSec(wm.vbusLimMs);
  ev.thermRegSec = powerMsToClampedSec(wm.thermRegMs);
  ev.tempCapSec  = powerMsToClampedSec(wm.tempCapMs);
  ev.screenOnSec = powerMsToClampedSec(wm.screenOnMs);
  ev.aodSec      = powerMsToClampedSec(wm.aodMs);
  ev.startPct    = wm.startPct;
}


// Machine-readable charge stage. Order is part of the wire protocol: keep it
// append-only (stat/BLE report this as a number).
enum class BattStage : uint8_t {
  NONE = 0,     // no battery / not readable
  DISCHARGE,    // on battery
  PRE,          // plugged, trickle pre-charge
  CC,           // plugged, constant current
  CV,           // plugged, constant voltage
  DONE,         // plugged, charge complete
  LIMITED,      // plugged, charging paused by policy (limit or temperature)
  USB_IDLE,     // plugged, not charging, no policy reason known
};

enum class ChargeSpeed : uint8_t { QUIET = 0, AUTO = 1, FAST = 2 };

// AXP2101 STATUS2[2:0] charger state, as exposed by XPowersLib.
enum : uint8_t {
  AXP_CHG_TRI  = 0,
  AXP_CHG_PRE  = 1,
  AXP_CHG_CC   = 2,
  AXP_CHG_CV   = 3,
  AXP_CHG_DONE = 4,
  AXP_CHG_STOP = 5,
};

constexpr uint16_t kBattDesignMah            = 600;   // Waveshare 1.8 board pack
constexpr uint8_t  kBattLimitMin             = 80;
constexpr uint8_t  kBattLimitMax             = 100;
constexpr uint8_t  kBattLimitHyst            = 4;     // resume charging at limit - this
constexpr uint16_t kBattVbusLimitMa          = 500;   // AXP2101 VBUS input current cap
constexpr uint16_t kBattSysActiveMa          = 180;   // typical system draw when screen/WiFi active
constexpr uint16_t kBattSysSleepMa           = 80;    // typical system draw when screen off / AOD
constexpr uint16_t kBattPrechargeMa          = 50;    // AXP2101 IPRECHG setting
constexpr uint16_t kBattTermMa               = 50;    // AXP2101 ITERM setting
constexpr uint32_t kBattCvTauMs              = 480000u; // 8-min CV current taper time constant
constexpr uint16_t kBattDischargeScreenOnMv  = 55;    // IR drop with active screen + WiFi
constexpr uint16_t kBattDischargeLowLoadMv   = 25;    // IR drop with screen off / AOD (~100mA)
constexpr uint16_t kBattChgPackMohm          = 620;   // calibrated from 1.8 real data (210mV @ 298mA)
constexpr uint16_t kBattChgPolarMv           = 20;    // active charging surface polarization
constexpr uint16_t kBattDonePolarMv          = 20;    // residual polarization at DONE

struct BatteryCal {
  uint16_t chgMohm     = kBattChgPackMohm; // [200..1200] mOhm charge-path ESR
  uint16_t designMah   = kBattDesignMah;   // [400..950] mAh effective pack capacity
  uint32_t cvTauMs     = kBattCvTauMs;     // [240000..900000] ms CV taper tau
  uint16_t fullRestMv  = 4180;             // [4120..4220] mV observed full rest OCV
  uint16_t lowAnchorMv = 3400;             // [3350..3520] mV empty cutoff anchor
  uint16_t rSamples    = 0;
  uint16_t capSamples  = 0;
  uint16_t cvSamples   = 0;
};

struct BatteryIn {
  uint16_t mV             = 0;
  bool     usb            = false;
  bool     charging       = false;
  uint8_t  chgState       = AXP_CHG_STOP;
  int      gaugePct       = -1;     // -1 = invalid / absent
  float    tempC          = NAN;
  uint32_t dtMs           = 1000;
  uint16_t targetMa       = 300;    // governor output, for the time model
  bool     limited        = false;  // governor paused charging
  uint8_t  limitPct       = 100;    // charge cap (80/90/100)
  bool     screenOn       = true;   // load / VBUS budget hint
  int16_t  seedPct        = -1;     // persisted boot pct anchor (-1 = none)
  bool     seedPowerLost  = false;  // true if PMU lost power (e.g. overnight cutoff)
  bool     seedMvValid    = false;  // true if seedMv was sampled with charging paused
  uint16_t seedMv         = 0;      // unloaded boot OCV sample (mV)
  uint16_t calChgMohm     = 0;      // 0 = use kBattChgPackMohm
  uint16_t calDesignMah   = 0;      // 0 = use kBattDesignMah
  uint32_t calCvTauMs     = 0;      // 0 = use kBattCvTauMs
  uint16_t calLowAnchorMv = 0;      // 0 = use 3400 mV
  uint16_t probeOcvMv     = 0;      // >0 when a 1s unloaded OCV probe just completed
};

struct BatteryOut {
  uint8_t   pct    = 0;
  uint8_t   rawPct = 0;
  uint16_t  compMv = 0;
  uint16_t  effMa  = 0;
  BattStage stage  = BattStage::NONE;
  bool      full   = false;
};

struct BatteryModel {
  bool      init      = false;
  uint8_t   pct       = 0;
  uint8_t   target    = 0;
  uint8_t   rawPct    = 0;
  BattStage stage     = BattStage::NONE;
  bool      full      = false;
  bool      chargeWas = false;

  uint16_t  smoothMv  = 0;
  uint32_t  accumMs   = 0;   // generic slew accumulator
  uint32_t  cvMs      = 0;   // time spent in CV
  uint32_t  lowMs     = 0;   // time below the empty threshold
  int32_t   chgMilli  = 0;   // time-model estimate, milli-percent (44000 = 44%)
  bool      chgPrimed = false;
};

// Piecewise-linear OCV curve for a 4.2V single-cell LiPo at ~0.2C.
// Endpoint pairs are (mV, percent); every percent point must be <= the one
// above it (the test asserts this).
struct OcvPoint { uint16_t mv; uint8_t pct; };
constexpr OcvPoint kBattOcv[] = {
  { 4200, 100 }, { 4150, 96 }, { 4110, 92 }, { 4080, 88 }, { 4040, 83 },
  { 4000, 77 },  { 3960, 70 }, { 3910, 62 }, { 3860, 54 }, { 3810, 46 },
  { 3760, 38 },  { 3710, 30 }, { 3660, 23 }, { 3610, 16 }, { 3560, 10 },
  { 3500, 5 },   { 3430, 1 },  { 3350, 0 },
};
constexpr int kBattOcvCount = (int)(sizeof(kBattOcv) / sizeof(kBattOcv[0]));

inline uint8_t battOcvPct(int mv) {
  if (mv >= (int)kBattOcv[0].mv) return 100;
  if (mv <= (int)kBattOcv[kBattOcvCount - 1].mv) return 0;
  for (int i = 0; i + 1 < kBattOcvCount; i++) {
    const int hiMv = (int)kBattOcv[i].mv;
    const int loMv = (int)kBattOcv[i + 1].mv;
    if (mv <= hiMv && mv >= loMv) {
      const int hiP = (int)kBattOcv[i].pct;
      const int loP = (int)kBattOcv[i + 1].pct;
      const int span = hiMv - loMv;
      if (span <= 0) return (uint8_t)hiP;
      return (uint8_t)(loP + ((mv - loMv) * (hiP - loP) + span / 2) / span);
    }
  }
  return 0;
}

inline uint16_t battPctToOcvMv(uint8_t pct) {
  if (pct >= 100) return kBattOcv[0].mv;
  for (int i = 0; i + 1 < kBattOcvCount; i++) {
    const int hiP = (int)kBattOcv[i].pct;
    const int loP = (int)kBattOcv[i + 1].pct;
    if ((int)pct <= hiP && (int)pct >= loP) {
      const int hiMv = (int)kBattOcv[i].mv;
      const int loMv = (int)kBattOcv[i + 1].mv;
      const int span = hiP - loP;
      if (span <= 0) return (uint16_t)hiMv;
      return (uint16_t)(loMv + (((int)pct - loP) * (hiMv - loMv) + span / 2) / span);
    }
  }
  return kBattOcv[kBattOcvCount - 1].mv;
}

inline bool battMvValid(uint16_t mv) { return mv >= 2500 && mv <= 5000; }
inline bool battTempValid(float c) { return !isnan(c) && c > -20.0f && c < 90.0f; }

// Milliseconds the model needs per 1% move, by direction and stage.
inline uint32_t battStepMs(BattStage s, bool up) {
  if (!up) return 10000;                       // discharge / cap drop: max 1%/10s
  switch (s) {
    case BattStage::DONE: return 5000;         // finishing walk to 100%/cap
    default:              return 25000;        // PRE/CC/CV/USB_IDLE: max 1%/25s (never sprint 4% in 12s)
  }
}

inline bool battIsChargingStage(BattStage s) {
  return s == BattStage::PRE || s == BattStage::CC || s == BattStage::CV ||
         s == BattStage::DONE;
}

// Effective battery charge current after accounting for the 500mA VBUS input
// budget shared with VSYS, plus CV current tapering (time + Ohm's law bound).
inline uint16_t battEffectiveChargeMa(const BatteryIn& in, BattStage stage, uint32_t cvMs,
                                      uint8_t estPct = 0, uint16_t smoothMv = 0,
                                      uint16_t chgMohm = kBattChgPackMohm) {
  if (stage == BattStage::PRE) return kBattPrechargeMa;
  if (stage != BattStage::CC && stage != BattStage::CV) return 0;

  const uint16_t sysMa = in.screenOn ? kBattSysActiveMa : kBattSysSleepMa;
  const uint16_t maxVbusChg = (kBattVbusLimitMa > sysMa) ? (uint16_t)(kBattVbusLimitMa - sysMa) : 0;
  uint16_t ccMa = (in.targetMa < maxVbusChg) ? in.targetMa : maxVbusChg;

  if (stage == BattStage::CC) return ccMa;
  if (ccMa <= kBattTermMa) return ccMa;
  const uint32_t tauMs = (in.calCvTauMs >= 240000u && in.calCvTauMs <= 900000u)
                           ? in.calCvTauMs : kBattCvTauMs;
  uint16_t timeMa = (uint16_t)(kBattTermMa +
                               (uint64_t)(ccMa - kBattTermMa) * tauMs / (tauMs + cvMs));
  if (smoothMv > 0 && chgMohm >= 200u) {
    const uint16_t vOcv = battPctToOcvMv(estPct);
    const int headMv = (int)smoothMv - (int)vOcv - (int)kBattChgPolarMv;
    uint16_t ohmMa = (headMv > 0) ? (uint16_t)((headMv * 1000) / (int)chgMohm) : kBattTermMa;
    if (ohmMa < kBattTermMa) ohmMa = kBattTermMa;
    if (ohmMa < timeMa) timeMa = ohmMa;
  }
  return timeMa;
}

inline void battCalSanitize(BatteryCal& cal) {
  if (cal.chgMohm < 200u) cal.chgMohm = 200u;
  if (cal.chgMohm > 1200u) cal.chgMohm = 1200u;
  if (cal.designMah < 400u) cal.designMah = 400u;
  if (cal.designMah > 950u) cal.designMah = 950u;
  if (cal.cvTauMs < 240000u) cal.cvTauMs = 240000u;
  if (cal.cvTauMs > 900000u) cal.cvTauMs = 900000u;
  if (cal.fullRestMv < 4120u) cal.fullRestMv = 4120u;
  if (cal.fullRestMv > 4220u) cal.fullRestMv = 4220u;
  if (cal.lowAnchorMv < 3350u) cal.lowAnchorMv = 3350u;
  if (cal.lowAnchorMv > 3520u) cal.lowAnchorMv = 3520u;
}

// Learn charge-path ESR from a brief unloaded OCV probe during CC (<4185mV).
inline bool battCalOnChargePause(BatteryCal& cal, uint16_t vChgMv,
                                 uint16_t vRestMv, uint16_t ma) {
  if (!battMvValid(vChgMv) || !battMvValid(vRestMv)) return false;
  if (ma < 150u || ma > 500u) return false;
  if (vChgMv >= 4185u || vChgMv <= vRestMv + 25u) return false;
  const int dv = (int)vChgMv - (int)vRestMv - (int)kBattChgPolarMv;
  if (dv < 10) return false;

  int measured = (dv * 1000 + (int)ma / 2) / (int)ma;
  if (measured < 200) measured = 200;
  if (measured > 1200) measured = 1200;

  const int w = (cal.rSamples < 3u) ? 40 : 20;
  cal.chgMohm = (uint16_t)(((int)cal.chgMohm * (100 - w) + measured * w + 50) / 100);
  cal.rSamples++;
  battCalSanitize(cal);
  return true;
}

// Learn effective pack capacity from two unloaded OCV anchors across a clean CC span.
inline bool battCalOnCapacitySpan(BatteryCal& cal, uint16_t startOcvMv,
                                  uint16_t endOcvMv, uint32_t chargedUah) {
  if (chargedUah < 50000u) return false;
  if (startOcvMv < 3520u || endOcvMv > 4060u || endOcvMv <= startOcvMv) return false;
  const int soc0 = (int)battOcvPct(startOcvMv);
  const int soc1 = (int)battOcvPct(endOcvMv);
  const int dSoc = soc1 - soc0;
  if (dSoc < 12) return false;

  int measuredMah = (int)(((uint64_t)chargedUah * 95u) / ((uint64_t)dSoc * 1000u));
  if (measuredMah < 400) measuredMah = 400;
  if (measuredMah > 950) measuredMah = 950;

  const int w = (cal.capSamples < 3u) ? 30 : 15;
  cal.designMah = (uint16_t)(((int)cal.designMah * (100 - w) + measuredMah * w + 50) / 100);
  cal.capSamples++;
  battCalSanitize(cal);
  return true;
}

// Adjust CV taper time constant when hardware DONE completes.
inline bool battCalOnChargeDone(BatteryCal& cal, uint8_t endModelPct,
                                uint32_t cvDurationMs, uint32_t pinned99Ms) {
  if (cvDurationMs < 300000u) return false;
  uint32_t target = cal.cvTauMs;
  if (endModelPct < 96u) {
    target = cal.cvTauMs * 120u / 100u;
  } else if (pinned99Ms >= 600000u) {
    target = cal.cvTauMs * 85u / 100u;
  }
  cal.cvTauMs = (cal.cvTauMs * 75u + target * 25u) / 100u;
  cal.cvSamples++;
  battCalSanitize(cal);
  return true;
}

inline bool battCalOnFullRest(BatteryCal& cal, uint16_t restMv) {
  if (restMv < 4120u || restMv > 4220u) return false;
  cal.fullRestMv = (uint16_t)(((int)cal.fullRestMv * 75 + (int)restMv * 25 + 50) / 100);
  battCalSanitize(cal);
  return true;
}

inline bool battCalOnUnexpectedCutoff(BatteryCal& cal, int16_t seedPct, bool seedPowerLost) {
  if (!seedPowerLost || seedPct < 6 || seedPct > 15) return false;
  if (cal.lowAnchorMv < 3520u) {
    cal.lowAnchorMv = (uint16_t)(cal.lowAnchorMv + 15u);
    battCalSanitize(cal);
    return true;
  }
  return false;
}

// One 1s tick. Returns the value to display + the stage for the UI/protocol.
inline BatteryOut batteryModelStep(BatteryModel& m, const BatteryIn& in) {
  BatteryOut out;
  out.pct    = m.pct;
  out.rawPct = m.rawPct;
  out.stage  = m.stage;
  out.full   = m.full;

  if (!battMvValid(in.mV)) {
    // No pack / I2C glitch: hold the last value, but don't pretend to charge.
    m.stage = BattStage::NONE;
    m.chargeWas = false;
    out.stage = m.stage;
    return out;
  }

  const uint32_t dt = (in.dtMs > 60000u) ? 60000u : in.dtMs;
  const bool firstTick = !m.init;

  // 1. Smooth the ADC a little. 0.35 is fast enough to follow a real step,
  //    slow enough to ignore WiFi TX bursts. On first tick, prefer the unloaded
  //    boot OCV sample if available.
  if (m.smoothMv == 0) {
    m.smoothMv = (firstTick && in.seedMvValid && battMvValid(in.seedMv)) ? in.seedMv : in.mV;
  } else {
    m.smoothMv = (uint16_t)((m.smoothMv * 65 + in.mV * 35) / 100);
  }

  // 2. Stage from the PMU, not from the voltage.
  BattStage stage;
  if (in.usb && in.charging) {
    if (in.chgState == AXP_CHG_TRI || in.chgState == AXP_CHG_PRE) stage = BattStage::PRE;
    else if (in.chgState == AXP_CHG_CV) stage = BattStage::CV;
    else if (in.chgState == AXP_CHG_DONE) stage = BattStage::DONE;
    else stage = BattStage::CC;
  } else if (in.usb && in.chgState == AXP_CHG_DONE) {
    stage = BattStage::DONE;
  } else if (!in.usb) {
    stage = BattStage::DISCHARGE;
  } else if (in.limited) {
    stage = BattStage::LIMITED;
  } else {
    stage = BattStage::USB_IDLE;
  }

  const uint16_t chgMohm = (in.calChgMohm >= 200u && in.calChgMohm <= 1200u)
                             ? in.calChgMohm : kBattChgPackMohm;
  const uint16_t designMah = (in.calDesignMah >= 400u && in.calDesignMah <= 950u)
                               ? in.calDesignMah : kBattDesignMah;
  const uint16_t lowAnchorMv = (in.calLowAnchorMv >= 3350u && in.calLowAnchorMv <= 3520u)
                                 ? in.calLowAnchorMv : 3400u;

  const uint32_t nextCvMs = (stage == BattStage::CV) ? (m.cvMs + dt)
                          : (stage == BattStage::CC || stage == BattStage::PRE ? m.cvMs : 0u);
  const uint8_t pctHint = m.init ? m.pct : (in.seedPct >= 0 && in.seedPct <= 100 ? (uint8_t)in.seedPct : 0u);
  const uint16_t effMa = battEffectiveChargeMa(in, stage, nextCvMs, pctHint,
                                               m.init ? m.smoothMv : 0u, chgMohm);

  // Stage-specific IR compensation. Note: LIMITED and USB_IDLE sit on VBUS with
  // ~0mA battery current, so they use 0mV offset (not +discharge or -charge).
  int compMv = (int)m.smoothMv;
  if (firstTick && in.seedMvValid && battMvValid(in.seedMv) && in.usb) {
    // Boot reading was taken while charging was briefly paused on VBUS.
    compMv = (int)in.seedMv;
  } else if (battMvValid(in.probeOcvMv) && in.usb) {
    // Periodic 1s unloaded OCV probe sample.
    compMv = (int)in.probeOcvMv;
  } else if (stage == BattStage::DISCHARGE) {
    compMv += (int)(in.screenOn ? kBattDischargeScreenOnMv : kBattDischargeLowLoadMv);
  } else if (stage == BattStage::PRE || stage == BattStage::CC || stage == BattStage::CV) {
    const int irMv = (int)(((uint32_t)effMa * chgMohm + 500u) / 1000u) +
                     (int)kBattChgPolarMv;
    compMv -= irMv;
  } else if (stage == BattStage::DONE) {
    compMv -= (int)kBattDonePolarMv;
  }
  if (compMv < 2500) compMv = 2500;
  if (compMv > 5000) compMv = 5000;
  const uint8_t raw = battOcvPct(compMv);
  m.rawPct = raw;

  // 3. Target percentage by stage.
  const int cap = (in.limitPct >= kBattLimitMin && in.limitPct < 100) ? in.limitPct : 100;
  int target = m.target;
  if (firstTick) {
    m.init = true;
    int initTarget = (int)raw;
    if (in.seedPct >= 0 && in.seedPct <= 100) {
      if (!in.seedPowerLost) {
        // PMU retained battery-backed RAM across reboot; keep continuity unless
        // the pack voltage diverged dramatically (e.g. battery swapped).
        const int diff = (int)raw - (int)in.seedPct;
        if (diff >= -20 && diff <= 20) {
          initTarget = (int)in.seedPct;
        }
      }
      // A low anchor is authoritative regardless of whether the PMU lost power.
      // AXP2101 keeps DATA_BUFFER alive on VBAT through a VSYS_UVLO cutoff, so
      // gating this guard on seedPowerLost alone let two real 2026-11-24 cutoffs
      // boot at 76% and 36% (anchors 0% / 3%) from a polarized charge terminal
      // voltage. compMv < 4000 still leaves a genuine full-pack swap detectable.
      if (in.seedPct <= 15 && stage != BattStage::DONE && stage != BattStage::CV &&
          compMv < 4000) {
        const int maxSeed = (int)in.seedPct + 4;
        if (initTarget > maxSeed) initTarget = maxSeed;
      }
    }
    // A pack already above the maintenance cap on USB must boot at the cap,
    // not show 96% while the governor says "paused at 90%".
    if (in.usb && initTarget > cap) initTarget = cap;
    target = initTarget;
    m.pct  = (uint8_t)target;
    if (stage == BattStage::PRE || stage == BattStage::CC || stage == BattStage::CV) {
      m.chgPrimed = true;
      m.chgMilli  = (int32_t)m.pct * 1000;
    }
  } else if (stage == BattStage::DISCHARGE) {
    target = raw;
    m.chgPrimed = false;
    m.cvMs = 0;
  } else if (stage == BattStage::LIMITED) {
    // Paused by policy: hold the displayed value, the pack is resting and its
    // terminal voltage will fall a bit — don't let that read as consumption.
    // But if the cap was just lowered below the current estimate, walk down
    // to it (10 s per point) instead of showing 95% next to a 90% cap.
    target = (m.pct > cap) ? cap : m.pct;
    m.chgPrimed = false;
    m.cvMs = 0;
  } else if (stage == BattStage::USB_IDLE) {
    target = raw;
    m.chgPrimed = false;
    m.cvMs = 0;
  } else if (stage == BattStage::PRE || stage == BattStage::CC || stage == BattStage::CV) {
    // Continuous coulomb/time integrator across PRE -> CC -> CV so entering CV
    // never jumps straight to 95% / 80% in seconds.
    if (!m.chgPrimed) {
      m.chgPrimed = true;
      m.chgMilli  = (int32_t)m.pct * 1000;
    }
    if (stage == BattStage::CV) m.cvMs += dt;

    const uint32_t mpPerSec = (uint32_t)effMa * 95u * 1000u /
                              ((uint32_t)designMah * 3600u);
    int32_t deltaMilli = (int32_t)(mpPerSec * dt / 1000u);

    const int estPct = (int)(m.chgMilli / 1000);
    if (stage == BattStage::PRE || stage == BattStage::CC) {
      if ((int)raw > estPct + 4) {
        const int32_t extraRate = ((int)raw > estPct + 12) ? 16 : 8;
        deltaMilli += (int32_t)(extraRate * (int32_t)dt / 1000);
      } else if (estPct > (int)raw + 14) {
        deltaMilli = 0;
      } else if (estPct > (int)raw + 8) {
        deltaMilli /= 2;
      }
    } else {
      // In CV, terminal voltage is clamped at 4.20V; only nudge softly upward if
      // the compensated reading indicates the integrator started well below reality.
      if ((int)raw > estPct + 6) {
        deltaMilli += (int32_t)(6 * (int32_t)dt / 1000);
      }
    }

    m.chgMilli += deltaMilli;
    if (battMvValid(in.probeOcvMv)) {
      // Periodic 1s unloaded probe anchor: gently correct accumulated drift without jumping.
      const int32_t probeMilli = (int32_t)battOcvPct((int)in.probeOcvMv) * 1000;
      int32_t corr = (probeMilli - m.chgMilli) / 2;
      if (corr > 3000) corr = 3000;
      if (corr < -3000) corr = -3000;
      m.chgMilli += corr;
    }
    const int chargeCeil = (cap < 100) ? cap : 99;
    const int32_t maxMilli = (int32_t)chargeCeil * 1000;
    if (m.chgMilli > maxMilli) m.chgMilli = maxMilli;
    if (m.chgMilli < 0) m.chgMilli = 0;

    target = (int)(m.chgMilli / 1000);
  } else if (stage == BattStage::DONE) {
    m.cvMs = 0;
    m.chgPrimed = false;
    target = cap;
  }

  // 4. Anchors.
  if (stage == BattStage::DISCHARGE) {
    if (m.smoothMv < lowAnchorMv) {
      m.lowMs += dt;
      if (m.lowMs > 10000u) target = 0;
    } else {
      m.lowMs = 0;
    }
  } else if (stage != BattStage::NONE) {
    m.lowMs = 0;
  }
  if (stage == BattStage::DONE) m.full = true;
  if (m.pct < 98 && m.full) m.full = false;

  // 5. Clamp + slew limit.
  if (target < 0) target = 0;
  if (target > 100) target = 100;
  m.target = (uint8_t)target;

  m.accumMs += dt;
  if (m.pct < (uint8_t)target) {
    // Rising while on battery means the load just dropped (OCV recovered).
    // That is not extra energy, so let it crawl back instead of jumping.
    const uint32_t stepMs = (stage == BattStage::DISCHARGE)
                              ? 60000u
                              : battStepMs(stage, true);
    while (m.pct < (uint8_t)target && m.accumMs >= stepMs) {
      m.accumMs -= stepMs;
      m.pct++;
    }
  } else if (m.pct > (uint8_t)target) {
    const uint32_t stepMs = battStepMs(stage, false);
    while (m.pct > (uint8_t)target && m.accumMs >= stepMs) {
      m.accumMs -= stepMs;
      m.pct--;
    }
  } else {
    m.accumMs = 0;
  }
  if (m.accumMs > 60000u) m.accumMs = 60000u;

  m.stage = stage;
  m.chargeWas = in.charging;

  out.pct    = m.pct;
  out.rawPct = m.rawPct;
  out.compMv = (uint16_t)compMv;
  out.effMa  = effMa;
  out.stage  = m.stage;
  out.full   = m.full;
  return out;
}

// ---------------------------------------------------------------------------
// Charge governor (policy only; the caller writes AXP registers).
// ---------------------------------------------------------------------------

struct ChargeGovernor {
  bool     init        = false;
  bool     tempCapped  = false;   // latched while hot, cleared with hysteresis
  bool     limitCut    = false;   // paused because the pack reached the limit
  uint32_t lastStepMs  = 0;
};

struct ChargeDecision {
  uint16_t ma         = 300;
  bool     enable     = true;
  bool     limited    = false;
  bool     tempCapped = false;
  bool     limitCut   = false;
};

// Speed table. AUTO is the shipping default; dim/screen state picks the row.
inline uint16_t chargeBaseMa(ChargeSpeed speed, bool screenOn) {
  switch (speed) {
    case ChargeSpeed::QUIET: return screenOn ? 200 : 300;
    case ChargeSpeed::FAST:  return 500;
    case ChargeSpeed::AUTO:
    default:                 return screenOn ? 300 : 500;
  }
}

// tempC is the AXP2101 die temperature (the pack NTC is not enabled yet).
// Thresholds are deliberately above ambient: the PMU sits next to the AMOLED.
inline ChargeDecision chargeGovernorStep(ChargeGovernor& g, ChargeSpeed speed,
                                         bool screenOn, float tempC, uint8_t pct,
                                         uint8_t limitPct, bool usb, uint32_t nowMs) {
  ChargeDecision d;
  if (!g.init) {
    g.init = true;
    g.lastStepMs = nowMs;
  }
  (void)nowMs;

  if (limitPct < kBattLimitMin) limitPct = kBattLimitMin;
  if (limitPct > kBattLimitMax) limitPct = kBattLimitMax;

  uint16_t ma = chargeBaseMa(speed, screenOn);

  // Temperature band with hysteresis. These are die temperatures, not pack
  // temperatures — the PMU sits next to the AMOLED, so it reads high.
  if (battTempValid(tempC)) {
    if (tempC >= 58.0f) g.tempCapped = true;
    else if (tempC < 44.0f) g.tempCapped = false;

    if (g.tempCapped) {
      ma = 0;
    } else if (tempC >= 52.0f) {
      if (ma > 100) ma = 100;
    } else if (tempC >= 48.0f) {
      if (ma > 200) ma = 200;
    }
  }

  // Charge-limit latch: cut at the cap, re-arm when unplugged (so the next
  // plug-in tops up to the cap again) or if the pack fell well below it.
  // limitPct == 100 means "no cap": let the PMU terminate on its own and
  // report DONE, instead of cutting at 100% and reporting a fake LIMITED.
  if (limitPct >= 100) g.limitCut = false;
  else if (!usb) g.limitCut = false;
  else if (pct >= limitPct) g.limitCut = true;
  else if ((int)pct <= (int)limitPct - (int)kBattLimitHyst) g.limitCut = false;

  if (g.limitCut) ma = 0;

  d.ma         = ma;
  d.enable     = ma > 0;
  d.limited    = !d.enable;
  d.tempCapped = g.tempCapped;
  d.limitCut   = g.limitCut;
  return d;
}
