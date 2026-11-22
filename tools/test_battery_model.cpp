// Host-side tests for src/model/battery_model.h.
// Build/run: bash tools/test_battery.sh
//
// No Arduino, no I2C: feed synthetic ticks and assert the contract the plan
// promises (monotonic display, anchors, slew limits, governor table).

#include "model/battery_model.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
  g_checks++; \
  if (!(cond)) { \
    g_fail++; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
  } \
} while (0)

#define CHECK_MSG(cond, ...) do { \
  g_checks++; \
  if (!(cond)) { \
    g_fail++; \
    std::printf("FAIL %s:%d  %s\n  ", __FILE__, __LINE__, #cond); \
    std::printf(__VA_ARGS__); \
    std::printf("\n"); \
  } \
} while (0)

static BatteryIn tickIn(uint16_t mv, bool usb, bool charging, uint8_t st,
                        uint16_t ma = 300, bool limited = false, float temp = NAN,
                        uint8_t limitPct = 100) {
  BatteryIn in;
  in.mV = mv;
  in.usb = usb;
  in.charging = charging;
  in.chgState = st;
  in.tempC = temp;
  in.dtMs = 1000;
  in.targetMa = ma;
  in.limited = limited;
  in.limitPct = limitPct;
  return in;
}

// Run N one-second ticks, returning the last output.
static BatteryOut run(BatteryModel& m, int n, BatteryIn in) {
  BatteryOut o;
  for (int i = 0; i < n; i++) o = batteryModelStep(m, in);
  return o;
}

static void testCurve() {
  // Monotonic and clamped over the whole usable range.
  uint8_t prev = 0;
  for (int mv = 3300; mv <= 4250; mv++) {
    uint8_t p = battOcvPct(mv);
    CHECK_MSG(p >= prev, "non-monotonic at %d mV: %u < %u", mv, p, prev);
    prev = p;
  }
  CHECK(battOcvPct(3350) == 0);
  CHECK(battOcvPct(4200) == 100);
  CHECK(battOcvPct(4300) == 100);
  CHECK(battOcvPct(3000) == 0);
  CHECK(battOcvPct(3860) > 50 && battOcvPct(3860) < 60);
}

static void testDischargeSlew() {
  BatteryModel m;
  // Settle at 4.02V -> ~80%.
  BatteryOut o = run(m, 5, tickIn(4020, false, false, AXP_CHG_STOP));
  CHECK_MSG(o.stage == BattStage::DISCHARGE, "stage %u", (unsigned)o.stage);
  const uint8_t start = o.pct;
  CHECK_MSG(start > 70 && start < 90, "start pct %u", start);

  // Step the voltage down hard (load spike): 3.80V -> ~44%+50mV.
  BatteryOut mid = run(m, 1, tickIn(3800, false, false, AXP_CHG_STOP));
  CHECK_MSG(mid.pct + 2 >= start, "one tick must not jump down: %u -> %u",
            start, mid.pct);

  BatteryOut after30 = run(m, 30, tickIn(3800, false, false, AXP_CHG_STOP));
  CHECK_MSG(after30.pct <= mid.pct && after30.pct >= mid.pct - 3,
            "slew should walk down 1%%/10s: %u -> %u -> %u",
            start, mid.pct, after30.pct);

  // 1%/10s at most => 120s after `mid` is at most 12 points.
  BatteryOut after2m = run(m, 90, tickIn(3800, false, false, AXP_CHG_STOP));
  CHECK_MSG(after2m.pct >= mid.pct - 12, "faster than 1%%/10s: %u -> %u",
            mid.pct, after2m.pct);

  // Load disappears: voltage back to 4.05V. It must NOT jump up.
  BatteryOut rec1 = run(m, 1, tickIn(4050, false, false, AXP_CHG_STOP));
  CHECK(rec1.pct <= after2m.pct + 1);
  BatteryOut rec60 = run(m, 60, tickIn(4050, false, false, AXP_CHG_STOP));
  CHECK_MSG(rec60.pct <= after2m.pct + 2, "recovery too fast: %u -> %u",
            after2m.pct, rec60.pct);
}

static void testChargeSequence() {
  BatteryModel m;
  BatteryOut o = run(m, 5, tickIn(3750, false, false, AXP_CHG_STOP));
  const uint8_t start = o.pct;
  CHECK_MSG(start > 25 && start < 50, "start %u", start);

  // Plug in, CC at 300mA. 600mAh => ~0.79%/min. 10 min => +7..9.
  BatteryOut cc = run(m, 600, tickIn(3900, true, true, AXP_CHG_CC, 300));
  CHECK_MSG(cc.stage == BattStage::CC, "stage %u", (unsigned)cc.stage);
  CHECK_MSG(cc.pct > start + 4 && cc.pct < start + 14,
            "CC in 10min: %u -> %u", start, cc.pct);

  // CV: voltage pinned, model walks 95..99.
  BatteryOut cv = run(m, 600, tickIn(4190, true, true, AXP_CHG_CV, 300));
  CHECK_MSG(cv.stage == BattStage::CV, "stage %u", (unsigned)cv.stage);
  CHECK(cv.pct >= 95);

  BatteryOut done = run(m, 200, tickIn(4200, true, true, AXP_CHG_DONE, 0));
  CHECK_MSG(done.stage == BattStage::DONE, "stage %u", (unsigned)done.stage);
  CHECK_MSG(done.pct == 100, "DONE pct %u", done.pct);
  CHECK(done.full);

  // Unplug: 4.17V resting. Must not fall off a cliff.
  BatteryOut unplug = run(m, 60, tickIn(4170, false, false, AXP_CHG_STOP));
  CHECK_MSG(unplug.stage == BattStage::DISCHARGE, "stage %u", (unsigned)unplug.stage);
  CHECK_MSG(unplug.pct >= 96, "unplug dropped to %u", unplug.pct);
}

static void testLimitedHold() {
  BatteryModel m;
  run(m, 5, tickIn(4050, true, true, AXP_CHG_CC, 300));
  BatteryOut hot = run(m, 120, tickIn(4100, true, true, AXP_CHG_CC, 300));
  const uint8_t held = hot.pct;

  // Governor pauses at the limit; voltage sags once charging stops.
  BatteryOut lim = run(m, 300, tickIn(4020, true, false, AXP_CHG_STOP, 0, true));
  CHECK_MSG(lim.stage == BattStage::LIMITED, "stage %u", (unsigned)lim.stage);
  CHECK_MSG(lim.pct >= held - 1, "LIMITED must hold, %u -> %u", held, lim.pct);
}

static void testLimitCap() {
  BatteryModel m;
  BatteryOut o = run(m, 5, tickIn(4050, true, true, AXP_CHG_CC, 300, false, NAN, 90));
  // CV must stop at the cap, not walk to 95+.
  o = run(m, 900, tickIn(4190, true, true, AXP_CHG_CV, 300, false, NAN, 90));
  CHECK_MSG(o.stage == BattStage::CV, "stage %u", (unsigned)o.stage);
  CHECK_MSG(o.pct <= 90 && o.pct >= 88, "CV cap pct %u", o.pct);

  // A "done" reported while a 90% cap is active still displays the cap.
  o = run(m, 300, tickIn(4200, true, false, AXP_CHG_DONE, 0, false, NAN, 90));
  CHECK_MSG(o.stage == BattStage::DONE, "stage %u", (unsigned)o.stage);
  CHECK_MSG(o.pct == 90, "DONE with cap pct %u", o.pct);
}

static void testGovernorReArm() {
  ChargeGovernor g;
  // Reach the cap while plugged.
  ChargeDecision d = chargeGovernorStep(g, ChargeSpeed::AUTO, true, 30.0f, 90, 90, true, 0);
  CHECK(!d.enable && g.limitCut);
  // Unplug re-arms, so the next plug-in tops up again instead of sitting cut.
  d = chargeGovernorStep(g, ChargeSpeed::AUTO, true, 30.0f, 89, 90, false, 0);
  CHECK(d.enable);
  d = chargeGovernorStep(g, ChargeSpeed::AUTO, true, 30.0f, 89, 90, true, 0);
  CHECK(d.enable && d.ma == 300);
}

static void testLowAnchor() {
  BatteryModel m;
  run(m, 5, tickIn(3450, false, false, AXP_CHG_STOP));
  BatteryOut low = run(m, 120, tickIn(3380, false, false, AXP_CHG_STOP));
  CHECK_MSG(low.pct == 0, "low anchor pct %u", low.pct);
}

static void testGovernor() {
  // Speed table.
  CHECK(chargeBaseMa(ChargeSpeed::AUTO, true) == 300);
  CHECK(chargeBaseMa(ChargeSpeed::AUTO, false) == 500);
  CHECK(chargeBaseMa(ChargeSpeed::QUIET, true) == 200);
  CHECK(chargeBaseMa(ChargeSpeed::QUIET, false) == 300);
  CHECK(chargeBaseMa(ChargeSpeed::FAST, false) == 500);

  // Limit latch: cut at 90, resume at 86.
  ChargeGovernor g;
  ChargeDecision d = chargeGovernorStep(g, ChargeSpeed::AUTO, true, 30.0f, 89, 90, true, 0);
  CHECK(d.enable && d.ma == 300 && !d.limited);
  d = chargeGovernorStep(g, ChargeSpeed::AUTO, true, 30.0f, 90, 90, true, 0);
  CHECK(!d.enable && d.limited);
  d = chargeGovernorStep(g, ChargeSpeed::AUTO, true, 30.0f, 88, 90, true, 0);
  CHECK(!d.enable);
  d = chargeGovernorStep(g, ChargeSpeed::AUTO, true, 30.0f, 86, 90, true, 0);
  CHECK(d.enable && d.ma == 300);

  // Temperature bands + hysteresis.
  ChargeGovernor h;
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 49.0f, 50, 100, true, 0);
  CHECK_MSG(d.ma == 200, "49C ma %u", d.ma);
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 53.0f, 50, 100, true, 0);
  CHECK_MSG(d.ma == 100, "53C ma %u", d.ma);
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 59.0f, 50, 100, true, 0);
  CHECK_MSG(!d.enable, "59C should cut");
  // Still hot: no early resume.
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 46.0f, 50, 100, true, 0);
  CHECK_MSG(!d.enable, "latched hot should stay cut at 46C");
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 43.0f, 50, 100, true, 0);
  CHECK_MSG(d.enable && d.ma == 500, "cooled ma %u", d.ma);

  // Bad temperature must not disable charging.
  ChargeGovernor n;
  d = chargeGovernorStep(n, ChargeSpeed::AUTO, false, NAN, 50, 100, true, 0);
  CHECK(d.enable && d.ma == 500);
}

int main() {
  testCurve();
  testDischargeSlew();
  testChargeSequence();
  testLimitedHold();
  testLimitCap();
  testGovernorReArm();
  testLowAnchor();
  testGovernor();

  if (g_fail) {
    std::printf("\n%d/%d checks failed\n", g_fail, g_checks);
    return 1;
  }
  std::printf("battery_model: %d checks passed\n", g_checks);
  return 0;
}
