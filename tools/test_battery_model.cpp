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
  BatteryOut cc = run(m, 600, tickIn(4030, true, true, AXP_CHG_CC, 300));
  CHECK_MSG(cc.stage == BattStage::CC, "stage %u", (unsigned)cc.stage);
  CHECK_MSG(cc.pct > start + 4 && cc.pct < start + 14,
            "CC in 10min: %u -> %u", start, cc.pct);

  // Continue CC to ~75%, then enter CV: CV must progress smoothly via tapering
  // current rather than jumping straight to 95%.
  BatteryModel cvm;
  run(cvm, 5, tickIn(4190, true, true, AXP_CHG_CC, 300)); // ~75%
  const uint8_t preCv = run(cvm, 60, tickIn(4200, true, true, AXP_CHG_CC, 300)).pct;
  BatteryOut cv30s = run(cvm, 30, tickIn(4205, true, true, AXP_CHG_CV, 300));
  CHECK_MSG(cv30s.stage == BattStage::CV, "stage %u", (unsigned)cv30s.stage);
  CHECK_MSG(cv30s.pct <= preCv + 1, "CV 30s must not jump: %u -> %u", preCv, cv30s.pct);

  BatteryOut cv30m = run(cvm, 1800, tickIn(4200, true, true, AXP_CHG_CV, 300));
  CHECK_MSG(cv30m.pct > preCv + 5 && cv30m.pct <= 99,
            "CV 30m should taper upward smoothly: %u -> %u", preCv, cv30m.pct);

  BatteryOut done = run(cvm, 300, tickIn(4200, true, true, AXP_CHG_DONE, 0));
  CHECK_MSG(done.stage == BattStage::DONE, "stage %u", (unsigned)done.stage);
  CHECK_MSG(done.pct == 100, "DONE pct %u", done.pct);
  CHECK(done.full);

  // Unplug: 4.17V resting. Must not fall off a cliff.
  BatteryOut unplug = run(cvm, 60, tickIn(4170, false, false, AXP_CHG_STOP));
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

  // Lowering the cap below the current estimate walks down to the cap.
  BatteryOut lower = run(m, 120, tickIn(4050, true, false, AXP_CHG_STOP, 0, true, NAN, 80));
  CHECK_MSG(lower.stage == BattStage::LIMITED, "stage %u", (unsigned)lower.stage);
  CHECK_MSG(lower.pct <= 84, "LIMITED above cap should walk down, got %u", lower.pct);
}

static void testBootOnDone() {
  // Regression: booting while the PMU already reports DONE used to start the
  // estimate at 0 because CV/DONE skipped the table and rawPct was still 0.
  BatteryModel m;
  BatteryOut o = run(m, 2, tickIn(4167, true, false, AXP_CHG_DONE, 0, false, NAN, 90));
  CHECK_MSG(o.stage == BattStage::DONE, "stage %u", (unsigned)o.stage);
  CHECK_MSG(o.pct == 90, "boot on a full pack must clamp to the cap, got %u%%", o.pct);

  // With the cap off it may show the real state.
  BatteryModel f;
  o = run(f, 2, tickIn(4167, true, false, AXP_CHG_DONE, 0, false, NAN, 100));
  CHECK_MSG(o.pct >= 88, "unlimited boot pct %u", o.pct);
}

static void testLimitCap() {
  BatteryModel m;
  // Boot near 76% and run CV with a 90% maintenance cap.
  BatteryOut o = run(m, 5, tickIn(4195, true, true, AXP_CHG_CC, 300, false, NAN, 90));
  CHECK_MSG(o.pct >= 74 && o.pct <= 82, "start pct %u", o.pct);
  o = run(m, 2700, tickIn(4205, true, true, AXP_CHG_CV, 300, false, NAN, 90));
  CHECK_MSG(o.stage == BattStage::CV, "stage %u", (unsigned)o.stage);
  CHECK_MSG(o.pct <= 90 && o.pct >= 88, "CV cap pct %u", o.pct);

  // A "done" reported while a 90% cap is active still displays the cap.
  o = run(m, 300, tickIn(4200, true, false, AXP_CHG_DONE, 0, false, NAN, 90));
  CHECK_MSG(o.stage == BattStage::DONE, "stage %u", (unsigned)o.stage);
  CHECK_MSG(o.pct == 90, "DONE with cap pct %u", o.pct);
}

static void testOvernightCutoffPlugIn() {
  // Regression: overnight empty shutdown (NVS recorded 2%, PMU DATA_BUFFER lost)
  // followed by plugging in USB with an immediate 3860mV charging polarization spike
  // must NOT jump to 50%.
  BatteryModel m;
  BatteryIn in = tickIn(3860, true, true, AXP_CHG_CC, 300);
  in.seedPct = 2;
  in.seedPowerLost = true;
  in.seedMvValid = true;
  in.seedMv = 3520; // unloaded OCV sample taken before enabling charging
  BatteryOut boot = batteryModelStep(m, in);
  CHECK_MSG(boot.pct <= 10, "overnight cutoff boot jumped to %u%%", boot.pct);

  // Even without unloaded seedMvValid, power-loss NVS guard keeps boot <= seed + 4.
  BatteryModel m2;
  BatteryIn in2 = tickIn(3860, true, true, AXP_CHG_CC, 300);
  in2.seedPct = 1;
  in2.seedPowerLost = true;
  BatteryOut boot2 = batteryModelStep(m2, in2);
  CHECK_MSG(boot2.pct <= 6, "overnight power-loss guard failed: %u%%", boot2.pct);

  // Normal soft reboot while PMU DATA_BUFFER stayed intact keeps continuity.
  BatteryModel m3;
  BatteryIn in3 = tickIn(4050, true, true, AXP_CHG_CC, 300);
  in3.seedPct = 64;
  in3.seedPowerLost = false;
  BatteryOut boot3 = batteryModelStep(m3, in3);
  CHECK_MSG(boot3.pct == 64, "soft reboot continuity broken: %u%%", boot3.pct);
}

static void testUvloCutoffPlugIn() {
  // Regression for the 2026-11-24 VSYS_UVLO cutoffs on D93468. Both boots woke
  // on vbus_insert with an intact PMU DATA_BUFFER (seedPowerLost=false), so the
  // power-loss guard never ran. The 200ms "unloaded" boot sample still carries
  // charge polarization: raw OCV read 36% / 76% while the anchor, the AXP fuel
  // gauge and the 29s rest probe all said 3-15%. A low anchor must win over the
  // charging terminal voltage even when the PMU did not lose power.
  {
    BatteryModel m;
    BatteryIn in = tickIn(3747, true, true, AXP_CHG_CC, 300);
    in.gaugePct = 3;
    in.seedPct = 3;
    in.seedPowerLost = false;
    in.seedMvValid = true;
    in.seedMv = 3747;
    BatteryOut boot = batteryModelStep(m, in);
    CHECK_MSG(boot.pct <= 7, "uvlo boot 12:40 jumped to %u%% (anchor 3%%)", boot.pct);
    // The clamp must hold on subsequent ticks too: a first-frame-only fix would
    // let the CC integrator bounce straight back up to the polarized OCV.
    BatteryOut after30 = run(m, 30, tickIn(3864, true, true, AXP_CHG_CC, 300));
    CHECK_MSG(after30.pct <= 12, "uvlo boot rebounded to %u%% after 30s", after30.pct);
  }
  {
    BatteryModel m;
    BatteryIn in = tickIn(3994, true, true, AXP_CHG_CC, 300);
    in.gaugePct = 10;
    in.seedPct = 0;
    in.seedPowerLost = false;
    in.seedMvValid = true;
    in.seedMv = 3994;
    BatteryOut boot = batteryModelStep(m, in);
    CHECK_MSG(boot.pct <= 4, "uvlo boot 12:16 jumped to %u%% (anchor 0%%)", boot.pct);
  }
  // Guard must not mask a genuine pack swap: a stale low anchor plus a real
  // 4.18V rest OCV is outside the low-anchor clamp and must still read full.
  {
    BatteryModel m;
    BatteryIn in = tickIn(4180, true, true, AXP_CHG_CC, 300);
    in.seedPct = 3;
    in.seedPowerLost = false;
    in.seedMvValid = true;
    in.seedMv = 4180;
    BatteryOut boot = batteryModelStep(m, in);
    CHECK_MSG(boot.pct >= 95, "full-pack swap wrongly clamped to %u%%", boot.pct);
  }
}

static void testRestedBootBeatsLoadSaggedSeed() {
  // 2026-11-24 17:20:28 (D93468), after the 1s rest window was put on the boot
  // path: the pack had been run to cutoff, the PMU anchor read 0%, and the
  // settled 1s sample read 3647mV = 21%. The old code clamped that to seed+4 =
  // 4% and under-reported by 17 points all evening. A settled rest sample is
  // better information than a load-sagged anchor, so it must win.
  {
    BatteryModel m;
    BatteryIn in = tickIn(3801, true, true, AXP_CHG_CC, 300);
    in.gaugePct = 5;
    in.seedPct = 0;
    in.seedPowerLost = false;
    in.seedMvValid = true;
    in.seedMvSettled = true;
    in.seedMv = 3647;
    BatteryOut boot = batteryModelStep(m, in);
    CHECK_MSG(boot.pct >= 18 && boot.pct <= 24,
              "rested boot should land near 21%%, got %u%%", boot.pct);
    // And it must not sprint: 30s of the same polarized terminal voltage may
    // raise the display a little but nowhere near the 45% the 3801mV terminal
    // voltage implies.
    BatteryOut after30 = run(m, 30, tickIn(3908, true, true, AXP_CHG_CC, 300));
    CHECK_MSG(after30.pct <= 28, "rested boot rebounded to %u%% after 30s", after30.pct);
  }
  // The short 200ms sample is still treated as polarized: it must keep the old
  // low-anchor clamp, otherwise the 76% / 36% boots come straight back.
  {
    BatteryModel m;
    BatteryIn in = tickIn(3801, true, true, AXP_CHG_CC, 300);
    in.gaugePct = 5;
    in.seedPct = 0;
    in.seedPowerLost = false;
    in.seedMvValid = true;
    in.seedMvSettled = false;
    in.seedMv = 3801;
    BatteryOut boot = batteryModelStep(m, in);
    CHECK_MSG(boot.pct <= 4, "unsettled boot should clamp to seed+4, got %u%%", boot.pct);
  }
}

static void testChargeRampMatchesPackCapacity() {
  // 2026-11-24 17:20:57 -> 17:56:50 on 300mA: the display went 4 -> 72 in 36 min.
  // On the 600mAh spec figure that looked like a runaway (raw OCV led the rested
  // probe and the integrator sprinted). The pack is ~300mAh, where 300mA x
  // 0.598h = 179mAh really is ~60% of the pack. The regression to lock in is that
  // the ramp tracks the time model and the probes, never the polarized raw.
  BatteryModel m;
  BatteryCal cal;
  cal.designMah = 300;
  BatteryIn boot = tickIn(3801, true, true, AXP_CHG_CC, 300);
  boot.gaugePct = 5;
  boot.seedPct = 0;
  boot.seedPowerLost = false;
  boot.seedMvValid = true;
  boot.seedMvSettled = true;
  boot.seedMv = 3647;
  boot.calDesignMah = cal.designMah;
  boot.calChgMohm = cal.chgMohm;
  const uint8_t start = batteryModelStep(m, boot).pct;
  CHECK_MSG(start >= 18 && start <= 24, "start %u", start);

  // +12 min: rested probe 3869mV (55%), display should sit below 50%.
  BatteryIn t = tickIn(4044, true, true, AXP_CHG_CC, 300);
  t.calDesignMah = cal.designMah;
  t.calChgMohm = cal.chgMohm;
  t.probeOcvMv = 3869;
  BatteryOut mid = run(m, 723, t);
  CHECK_MSG(mid.pct >= 36 && mid.pct <= 52, "12min display %u (probe says 55%%)", mid.pct);

  // +24.5 min: rested probe 3944mV (67%), display should sit below 65% but close.
  BatteryIn t2 = tickIn(4094, true, true, AXP_CHG_CC, 300);
  t2.calDesignMah = cal.designMah;
  t2.calChgMohm = cal.chgMohm;
  t2.probeOcvMv = 3944;
  BatteryOut late = run(m, 721, t2);
  CHECK_MSG(late.pct >= 58 && late.pct <= 70, "24min display %u (probe says 67%%)", late.pct);
}

static void testNo76To80Jump() {
  // Regression: charging at 76% (whether CC stays or transitions to CV, or screen
  // turns off raising current 300->500mA) must never jump 4% in 10-30s.
  BatteryModel m;
  BatteryIn ccOn = tickIn(4201, true, true, AXP_CHG_CC, 300); // ~76% after -206mV
  ccOn.screenOn = true;
  BatteryOut b = run(m, 5, ccOn);
  const uint8_t startPct = b.pct;
  CHECK_MSG(startPct >= 74 && startPct <= 78, "start %u", startPct);

  // Screen turns off: governor asks 500mA (420mA effective after VBUS budget) and
  // terminal voltage stays pinned in CV at 4205mV. Over 30s, pct may rise at most 1%.
  BatteryIn cvOff = tickIn(4205, true, true, AXP_CHG_CV, 500);
  cvOff.screenOn = false;
  BatteryOut after30s = run(m, 30, cvOff);
  CHECK_MSG(after30s.pct <= startPct + 1,
            "charging jumped too fast in 30s: %u -> %u", startPct, after30s.pct);
}

static void testCompensationDetails() {
  // USB_IDLE uses 0mV offset (not +55mV discharge offset).
  BatteryModel idleM;
  BatteryOut idle = batteryModelStep(idleM, tickIn(4000, true, false, AXP_CHG_STOP));
  CHECK_MSG(idle.rawPct == 77, "USB_IDLE at 4000mV should be 77%%, got %u%%", idle.rawPct);

  // Real 1.8 hardware match: 4201mV @ 300mA charge -> compMv ~3995mV -> raw ~76%.
  BatteryModel chgM;
  BatteryIn cc300 = tickIn(4201, true, true, AXP_CHG_CC, 300);
  cc300.screenOn = true;
  BatteryOut chg = batteryModelStep(chgM, cc300);
  CHECK_MSG(chg.compMv >= 3985 && chg.compMv <= 4005, "300mA charge compMv %u", chg.compMv);
  CHECK_MSG(chg.rawPct >= 74 && chg.rawPct <= 78, "300mA rawPct %u", chg.rawPct);

  // Screen-off AOD discharge uses smaller IR drop than active bright screen.
  BatteryModel dOn, dOff;
  BatteryIn inOn = tickIn(3920, false, false, AXP_CHG_STOP);
  inOn.screenOn = true;
  BatteryIn inOff = tickIn(3920, false, false, AXP_CHG_STOP);
  inOff.screenOn = false;
  CHECK(batteryModelStep(dOn, inOn).compMv > batteryModelStep(dOff, inOff).compMv);
}

static void testSelfCalibration() {
  BatteryCal cal;
  // 1. Charge-pause ESR learning: at 300mA, 4120mV -> 3910mV resting => ~633mOhm.
  CHECK(battCalOnChargePause(cal, 4120, 3910, 300));
  CHECK(cal.rSamples == 1);
  CHECK_MSG(cal.chgMohm >= 580 && cal.chgMohm <= 680, "learned chgMohm %u", cal.chgMohm);
  // Reject CV-clamped (>=4185mV) or invalid pairs.
  CHECK(!battCalOnChargePause(cal, 4200, 3990, 300));
  CHECK(!battCalOnChargePause(cal, 4100, 3900, 50));

  // 2. Capacity learning across CC span: 3660mV (23%) -> 3860mV (54%) = 31% delta;
  //    at 700mAh real cell, 31% / 0.95 = 228.4mAh pushed -> learns toward ~700mAh.
  //    The default pack is 300mAh now, so a 700mAh cell must pull the estimate up
  //    from there: 300*0.7 + 700*0.3 = 420mAh on the first sample.
  BatteryCal capCal;
  CHECK(battCalOnCapacitySpan(capCal, 3660, 3860, 228400u));
  CHECK(capCal.capSamples == 1);
  CHECK_MSG(capCal.designMah > 405 && capCal.designMah < 440,
            "learned designMah %u", capCal.designMah);
  // Reject too-short delta (<12%).
  CHECK(!battCalOnCapacitySpan(capCal, 3810, 3860, 60000u));

  // 3. CV-to-DONE taper tuning.
  BatteryCal cvCal;
  const uint32_t origTau = cvCal.cvTauMs;
  CHECK(battCalOnChargeDone(cvCal, 93, 900000u, 0));
  CHECK_MSG(cvCal.cvTauMs > origTau, "tau should rise when DONE came before 96%%");
  CHECK(battCalOnFullRest(cvCal, 4160));
  CHECK(cvCal.fullRestMv < 4180);

  // 4. Unexpected cutoff at 8% nudges lowAnchorMv upward safely,
  //    but an explicit software shutdown without VSYS UVLO must not nudge it.
  BatteryCal swCal = cvCal;
  CHECK(!battCalOnUnexpectedCutoff(swCal, 8, true, 0x02));
  CHECK(swCal.lowAnchorMv == cvCal.lowAnchorMv);
  CHECK(battCalOnUnexpectedCutoff(cvCal, 8, true));
  CHECK_MSG(cvCal.lowAnchorMv == 3415, "lowAnchorMv %u", cvCal.lowAnchorMv);
  CHECK(battCalOnUnexpectedCutoff(cvCal, 18, true, 0x08));
  CHECK_MSG(cvCal.lowAnchorMv == 3430, "lowAnchorMv uvlo %u", cvCal.lowAnchorMv);

  // 5. Sanitizer clamps extreme values cleanly.
  BatteryCal bad{ 50, 100, 1000, 3900, 3100, 0, 0, 0 };
  battCalSanitize(bad);
  CHECK(bad.chgMohm == 200);
  CHECK(bad.designMah == 200);
  CHECK(bad.cvTauMs == 240000u);
  CHECK(bad.fullRestMv == 4120);
  CHECK(bad.lowAnchorMv == 3350);

  // 6. Periodic unloaded probe gently nudges model without jumping.
  BatteryModel pm;
  run(pm, 5, tickIn(4100, true, true, AXP_CHG_CC, 300)); // ~60%
  const uint8_t beforeProbe = pm.pct;
  BatteryIn probeIn = tickIn(4100, true, true, AXP_CHG_CC, 300);
  probeIn.probeOcvMv = 3960; // true OCV 70%
  BatteryOut afterProbe = batteryModelStep(pm, probeIn);
  CHECK_MSG(afterProbe.pct <= beforeProbe + 1, "probe must not jump immediately");
  CHECK_MSG(pm.target > beforeProbe && pm.target <= beforeProbe + 4,
            "probe nudged target gently: %u -> %u", beforeProbe, pm.target);

  // 7. CV Ohm's-law taper bound keeps tail rawPct accurate even right after reboot.
  BatteryModel tailCv;
  BatteryIn tailIn = tickIn(4211, true, true, AXP_CHG_CV, 300);
  tailIn.seedPct = 95;
  tailIn.seedPowerLost = false;
  BatteryOut tailOut = run(tailCv, 3, tailIn);
  CHECK_MSG(tailOut.effMa < 120, "95%% CV tail effMa should be tapered: %u", tailOut.effMa);
  CHECK_MSG(tailOut.rawPct >= 92, "95%% CV tail rawPct should stay high: %u", tailOut.rawPct);
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
  CHECK(!d.tempCapped);
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 53.0f, 50, 100, true, 0);
  CHECK_MSG(d.ma == 100, "53C ma %u", d.ma);
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 59.0f, 50, 100, true, 0);
  CHECK_MSG(!d.enable && d.tempCapped && !d.limitCut, "59C should cut via temp");
  // Still hot: no early resume.
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 46.0f, 50, 100, true, 0);
  CHECK_MSG(!d.enable && d.tempCapped, "latched hot should stay cut at 46C");
  d = chargeGovernorStep(h, ChargeSpeed::AUTO, false, 43.0f, 50, 100, true, 0);
  CHECK_MSG(d.enable && d.ma == 500 && !d.tempCapped, "cooled ma %u", d.ma);

  // limit 100 = no cap: no cut even at 100%.
  ChargeGovernor c;
  d = chargeGovernorStep(c, ChargeSpeed::AUTO, true, 30.0f, 100, 100, true, 0);
  CHECK(d.enable);
  CHECK_MSG(!d.limited, "limit 100 must not report LIMITED");

  // Bad temperature must not disable charging.
  ChargeGovernor n;
  d = chargeGovernorStep(n, ChargeSpeed::AUTO, false, NAN, 50, 100, true, 0);
  CHECK(d.enable && d.ma == 500);
}

static void testPowerWatermark() {
  auto eq = [](const char* a, const char* b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
  };

  PowerWatermark wm{};
  powerWatermarkReset(wm, false, 3950, 3920, 0, 28, 72);
  CHECK(wm.active && !wm.chargingSess && wm.startPct == 72);
  CHECK(wm.vbatMinMv == 3950 && wm.vbatMaxMv == 3950 && wm.vsysMinMv == 3920);
  CHECK(wm.vbusMinMv == 0 && wm.tmpMaxC == 28);

  // AOD active must accumulate aodMs instead of double-counting screenOnMs;
  // invalid temp -128 is ignored; dt > 10s clamps to 10s.
  powerWatermarkAccum(wm, 2000, false, 3780, 3740, 0, 35, false, false, false, true, false);
  powerWatermarkAccum(wm, 3000, false, 3810, 3760, 0, -128, false, false, false, true, true);
  powerWatermarkAccum(wm, 20000, true, 4120, 4150, 4890, 41, true, true, true, false, false);
  CHECK(wm.durMs == 15000u);
  CHECK(wm.screenOnMs == 2000u && wm.aodMs == 3000u);
  CHECK(wm.vbatMinMv == 3780 && wm.vbatMaxMv == 4120);
  CHECK(wm.vsysMinMv == 3740 && wm.vbusMinMv == 4890);
  CHECK(wm.tmpMaxC == 41);
  CHECK(wm.vbusLimMs == 10000u && wm.thermRegMs == 10000u && wm.tempCapMs == 10000u);

  PowerEvent ev{};
  ev.reason = BattPwrEvtReason::USB_IN;
  powerWatermarkAttach(wm, ev);
  CHECK(ev.hasWatermark && ev.durSec == 15u && ev.startPct == 72);
  CHECK(ev.vbatMinMv == 3780 && ev.vbatMaxMv == 4120 && ev.vsysMinMv == 3740);
  CHECK(ev.vbusMinMv == 4890 && ev.tmpMaxC == 41);
  CHECK(ev.screenOnSec == 2u && ev.aodSec == 3u && ev.vbusLimSec == 10u &&
        ev.thermRegSec == 10u && ev.tempCapSec == 10u);
  CHECK(powerMsToClampedSec(90000000u) == 65535u);
  CHECK(eq(battPwrEvtReasonStr(BattPwrEvtReason::BOOT), "boot"));
  CHECK(eq(battPwrEvtReasonStr(BattPwrEvtReason::PROBE), "probe"));
  CHECK(eq(battPwrEvtReasonStr(BattPwrEvtReason::THERM), "therm"));
  CHECK(eq(battPwrEvtReasonStr(BattPwrEvtReason::LOW_BAT), "low_bat"));
}

static void testChgLedPolicy() {
  // Daytime on USB: let the charger drive CHGLED normally.
  CHECK(chgLedPolicyFor(true, true, 14, true, false, true) == ChgLedPolicy::AUTO_CHG);
  CHECK(chgLedPolicyFor(true, true, 8, false, true, true) == ChgLedPolicy::AUTO_CHG);
  // Quiet night hours (23:00-08:00) mute the LED when quietHours is enabled.
  CHECK(chgLedPolicyFor(true, true, 23, true, false, true) == ChgLedPolicy::OFF);
  CHECK(chgLedPolicyFor(true, true, 2, false, true, true) == ChgLedPolicy::OFF);
  CHECK(chgLedPolicyFor(true, true, 7, true, false, true) == ChgLedPolicy::OFF);
  // Disabling quietHours or lacking valid RTC keeps daytime indicator active on USB.
  CHECK(chgLedPolicyFor(false, true, 2, true, false, true) == ChgLedPolicy::AUTO_CHG);
  CHECK(chgLedPolicyFor(true, false, 2, true, false, true) == ChgLedPolicy::AUTO_CHG);
  // Unplugged deep idle (screen off, no AOD) mutes LED even during the day.
  CHECK(chgLedPolicyFor(true, true, 14, false, false, false) == ChgLedPolicy::OFF);
  CHECK(chgLedPolicyFor(true, true, 14, true, false, false) == ChgLedPolicy::AUTO_CHG);
}

int main() {
  testCurve();
  testDischargeSlew();
  testChargeSequence();
  testLimitedHold();
  testBootOnDone();
  testLimitCap();
  testOvernightCutoffPlugIn();
  testUvloCutoffPlugIn();
  testRestedBootBeatsLoadSaggedSeed();
  testChargeRampMatchesPackCapacity();
  testNo76To80Jump();
  testCompensationDetails();
  testSelfCalibration();
  testGovernorReArm();
  testLowAnchor();
  testGovernor();
  testPowerWatermark();
  testChgLedPolicy();

  if (g_fail) {
    std::printf("\n%d/%d checks failed\n", g_fail, g_checks);
    return 1;
  }
  std::printf("battery_model: %d checks passed\n", g_checks);
  return 0;
}
