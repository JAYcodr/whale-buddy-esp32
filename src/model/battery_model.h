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

constexpr uint16_t kBattDesignMah   = 600;   // Waveshare 1.8 board pack
constexpr uint8_t  kBattLimitMin    = 80;
constexpr uint8_t  kBattLimitMax    = 100;
constexpr uint8_t  kBattLimitHyst   = 4;     // resume charging at limit - this
constexpr uint16_t kBattDischargeCompMv = 50;   // load IR drop
constexpr uint16_t kBattChargeCompMv    = 70;   // CC terminal voltage offset

struct BatteryIn {
  uint16_t mV        = 0;
  bool     usb       = false;
  bool     charging  = false;
  uint8_t  chgState  = AXP_CHG_STOP;
  int      gaugePct  = -1;     // -1 = invalid / absent
  float    tempC     = NAN;
  uint32_t dtMs      = 1000;
  uint16_t targetMa  = 300;    // governor output, for the time model
  bool     limited   = false;  // governor paused charging
  uint8_t  limitPct  = 100;    // charge cap (80/90/100)
};

struct BatteryOut {
  uint8_t   pct    = 0;
  uint8_t   rawPct = 0;
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

inline bool battMvValid(uint16_t mv) { return mv >= 2500 && mv <= 5000; }
inline bool battTempValid(float c) { return !isnan(c) && c > -20.0f && c < 90.0f; }

// Milliseconds the model needs per 1% move, by direction and stage.
inline uint32_t battStepMs(BattStage s, bool up) {
  if (!up) return 10000;                       // discharge: max 1%/10s
  switch (s) {
    case BattStage::DONE: return 1000;         // finishing is allowed to be quick
    case BattStage::CV:   return 3000;
    default:              return 3000;         // CC/PRE/USB_IDLE
  }
}

inline bool battIsChargingStage(BattStage s) {
  return s == BattStage::PRE || s == BattStage::CC || s == BattStage::CV ||
         s == BattStage::DONE;
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

  // 1. Smooth the ADC a little. 0.35 is fast enough to follow a real step,
  //    slow enough to ignore WiFi TX bursts.
  if (m.smoothMv == 0) m.smoothMv = in.mV;
  else m.smoothMv = (uint16_t)((m.smoothMv * 65 + in.mV * 35) / 100);

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

  // Metered stage (for rawPct compensation): CV pins the voltage at 4.2V, so
  // the table is meaningless there and the time model owns the number.
  bool useTable = stage == BattStage::DISCHARGE || stage == BattStage::USB_IDLE ||
                  stage == BattStage::PRE || stage == BattStage::CC ||
                  stage == BattStage::LIMITED;

  int compMv = (int)m.smoothMv + (int)kBattDischargeCompMv;
  if (stage == BattStage::PRE || stage == BattStage::CC) compMv -= (int)kBattChargeCompMv;
  if (compMv < 2500) compMv = 2500;
  if (compMv > 5000) compMv = 5000;
  const uint8_t raw = useTable ? battOcvPct(compMv) : m.rawPct;
  m.rawPct = raw;

  // 3. Target percentage by stage.
  const int cap = (in.limitPct >= kBattLimitMin && in.limitPct < 100) ? in.limitPct : 100;
  int target = m.target;
  if (!m.init) {
    m.init = true;
    target = raw;
    m.pct = raw;
  } else if (stage == BattStage::DISCHARGE) {
    target = raw;
    m.chgPrimed = false;
  } else if (stage == BattStage::LIMITED) {
    // Paused by policy: hold the displayed value, the pack is resting and its
    // terminal voltage will fall a bit — don't let that read as consumption.
    target = m.pct;
    m.chgPrimed = false;
  } else if (stage == BattStage::USB_IDLE) {
    target = raw;
    m.chgPrimed = false;
  } else if (stage == BattStage::PRE || stage == BattStage::CC) {
    // Time model: mA * 0.95 / capacity. 300mA on 600mAh = 47.5%/h = 0.79%/min.
    if (!m.chgPrimed) {
      m.chgPrimed = true;
      m.chgMilli  = (int32_t)m.pct * 1000;
    }
    // milli-percent per second, integer-exact: 13 at 300mA / 600mAh.
    const uint32_t mpPerSec = (uint32_t)in.targetMa * 95u * 1000u /
                              ((uint32_t)kBattDesignMah * 3600u);
    m.chgMilli += (int32_t)(mpPerSec * dt / 1000u);
    int timed = (int)(m.chgMilli / 1000);
    if (timed > 99) timed = 99;
    // The table still corrects a wrong starting point, but cannot drag the
    // display up faster than the pack is actually filling.
    int rawUse = (int)raw;
    if (rawUse > timed + 6) rawUse = timed + 6;
    target = (timed > rawUse) ? timed : rawUse;
    const int ccCap = (cap < 100) ? cap : 99;
    if (target > ccCap) target = ccCap;
  } else if (stage == BattStage::CV) {
    m.cvMs += dt;
    int floorPct = 95;
    if ((int)m.pct > floorPct) floorPct = m.pct;
    int cv = floorPct + (int)(m.cvMs / 120000u);   // +1% every 2 min
    target = (cv > 99) ? 99 : cv;
    if (target > cap) target = cap;
  } else if (stage == BattStage::DONE) {
    m.cvMs = 0;
    target = cap;
  }

  // 4. Anchors.
  if (stage == BattStage::DISCHARGE) {
    m.cvMs = 0;
    if (m.smoothMv < 3400) {
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
  uint16_t ma      = 300;
  bool     enable  = true;
  bool     limited = false;
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
  if (!usb) g.limitCut = false;
  else if (pct >= limitPct) g.limitCut = true;
  else if ((int)pct <= (int)limitPct - (int)kBattLimitHyst) g.limitCut = false;

  if (g.limitCut) ma = 0;

  d.ma      = ma;
  d.enable  = ma > 0;
  d.limited = !d.enable;
  return d;
}
