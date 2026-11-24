import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  createPowerStore,
  decodePowerOffSource,
  decodePowerOnSource,
  stageNameOf,
} from '../lib/power.js';

describe('Host power store & bounded analytics', () => {
  it('decodes AXP2101 blackbox sources and stage codes accurately', () => {
    assert.deepEqual(decodePowerOnSource(0x04), ['vbus_insert']);
    assert.deepEqual(decodePowerOnSource(0x05), ['pwron_low', 'vbus_insert']);
    assert.deepEqual(decodePowerOffSource(0x08), ['vsys_uvlo']);
    assert.deepEqual(decodePowerOffSource(0x82), ['sw_off', 'over_temp']);
    assert.equal(stageNameOf(3), 'CC');
    assert.equal(stageNameOf(4), 'CV');
    assert.equal(stageNameOf(6), 'LIMITED');
  });

  it('enforces strict log rotation cap and ignores 30s stat heartbeats in jsonl', async () => {
    const home = await mkdtemp(join(tmpdir(), 'nova-pwr-rot-'));
    const store = createPowerStore({
      dshHome: home,
      maxLogBytes: 4096,
      maxCycles: 4,
      statPersistMs: 60 * 60 * 1000,
    });

    // 30s stat ticks must update lastStat without inflating power-events.jsonl
    for (let i = 0; i < 20; i += 1) {
      store.recordStat('D93468', {
        evt: 'stat',
        mv: 4100 + i,
        pct: 80,
        chg: 1,
        stage: 3,
        tmp: 31,
        lcd: 1,
        aod: 0,
        brt: 80,
        mhz: 240,
        rssi: -40,
      }, 1_000_000 + i * 30_000);
    }

    // Flood > 60 keyframes (> 4096B) to trigger log rotation into .1.jsonl
    for (let i = 1; i <= 65; i += 1) {
      store.recordPowerEvent('D93468', {
        evt: 'pwr',
        seq: i,
        why: 'probe',
        pct: 50 + (i % 40),
        raw: 50,
        gauge: 48,
        st: 3,
        mv: 4050,
        cmv: 3860,
        rmv: 3865,
        vb: 5120,
        vs: 4220,
        ma: 300,
        ema: 300,
        tmp: 33,
        usb: 1,
        chg: 1,
        mohm: 620,
        mah: 600,
      }, 2_000_000 + i * 1000);
    }
    await store.flush(2_100_000);

    const disk = store.storageStats();
    assert.ok(disk.logBytes > 0 && disk.logBytes <= 4096, `logBytes=${disk.logBytes}`);
    assert.ok(disk.archiveBytes > 0 && disk.archiveBytes <= 4096, `archiveBytes=${disk.archiveBytes}`);
    assert.ok(disk.logBytes + disk.archiveBytes <= 8192);

    const activeLines = (await readFile(join(home, 'nova-buddy', 'power-events.jsonl'), 'utf8'))
      .trim()
      .split('\n');
    assert.ok(activeLines.every((l) => JSON.parse(l).evt === 'pwr'));

    // Reloading in a new instance restores exact persisted profile & continues byte tracking
    const reloaded = createPowerStore({ dshHome: home, maxLogBytes: 4096 });
    const rep = reloaded.getReport('D93468');
    assert.equal(rep.counters.events, 65);
    assert.equal(rep.lastStat.mv, 4119);
    const activeStat = await stat(join(home, 'nova-buddy', 'power-events.jsonl'));
    assert.equal(reloaded.storageStats().logBytes, activeStat.size);
  });

  it('computes drain rates, charge bottlenecks, resistance drift, and blackbox alerts', async () => {
    const home = await mkdtemp(join(tmpdir(), 'nova-pwr-analytics-'));
    const store = createPowerStore({ dshHome: home });
    const t0 = 1_700_000_000_000;

    // Boot blackbox showing previous VSYS UVLO cutoff + power loss
    store.recordPowerEvent('D93468', {
      evt: 'pwr',
      seq: 1,
      why: 'boot',
      pct: 90,
      raw: 90,
      gauge: 86,
      st: 1,
      mv: 4080,
      cmv: 4080,
      rmv: 4085,
      vb: 0,
      vs: 4070,
      ma: 0,
      ema: 0,
      tmp: 28,
      usb: 0,
      chg: 0,
      mohm: 620,
      mah: 600,
      onSrc: 0x04,
      offSrc: 0x08,
      seed: -1,
      lost: 1,
    }, t0);

    // 1) Screen-on discharge cycle: 1h, 90% -> 65% (-25%/h), 50m screen-on
    store.recordPowerEvent('D93468', {
      evt: 'pwr',
      seq: 2,
      why: 'usb_in',
      pct: 65,
      raw: 64,
      gauge: 60,
      st: 3,
      mv: 3920,
      cmv: 3920,
      vb: 5050,
      vs: 4180,
      ma: 300,
      ema: 300,
      tmp: 32,
      usb: 1,
      chg: 1,
      mohm: 620,
      mah: 600,
      wm: {
        dur: 3600,
        sp: 90,
        vmin: 3820,
        vmax: 4080,
        vsmin: 3800,
        tmax: 35,
        scrS: 3000,
        aodS: 200,
      },
    }, t0 + 3600_000);

    // 2) Weak-cable thermal-throttled charge cycle: 1h, 65% -> 95% (+30%/h), vbmin=4420, vlimS=1800, tregS=200
    store.recordPowerEvent('D93468', {
      evt: 'pwr',
      seq: 3,
      why: 'usb_out',
      pct: 95,
      raw: 94,
      gauge: 90,
      st: 1,
      mv: 4130,
      cmv: 4130,
      vb: 0,
      vs: 4120,
      ma: 0,
      ema: 0,
      tmp: 47,
      usb: 0,
      chg: 0,
      mohm: 620,
      mah: 600,
      wm: {
        dur: 3600,
        sp: 65,
        vmin: 3900,
        vmax: 4208,
        vsmin: 3890,
        vbmin: 4420,
        tmax: 47,
        vlimS: 1800,
        tregS: 200,
        tcapS: 0,
        scrS: 100,
        aodS: 0,
      },
    }, t0 + 7200_000);

    // 3) AOD-dominant discharge cycle: 2h, 95% -> 75% (-10%/h), 1.6h AOD
    store.recordPowerEvent('D93468', {
      evt: 'pwr',
      seq: 4,
      why: 'usb_in',
      pct: 75,
      raw: 75,
      gauge: 72,
      st: 3,
      mv: 3980,
      cmv: 3980,
      vb: 5120,
      vs: 4220,
      ma: 300,
      ema: 300,
      tmp: 30,
      usb: 1,
      chg: 1,
      mohm: 620,
      mah: 600,
      wm: {
        dur: 7200,
        sp: 95,
        vmin: 3940,
        vmax: 4130,
        vsmin: 3920,
        tmax: 33,
        scrS: 100,
        aodS: 5800,
      },
    }, t0 + 14_400_000);

    // 4) Three OCV probes tracking resistance drift +180 mΩ & fuel gauge bias
    for (const [idx, mohm] of [[5, 680], [6, 740], [7, 800]]) {
      store.recordPowerEvent('D93468', {
        evt: 'pwr',
        seq: idx,
        why: 'probe',
        pct: 80,
        raw: 79,
        gauge: 75,
        st: 3,
        mv: 4180,
        cmv: 4000,
        rmv: 4005,
        vb: 5100,
        vs: 4210,
        ma: 300,
        ema: 280,
        tmp: 34,
        usb: 1,
        chg: 1,
        mohm,
        mah: 600,
      }, t0 + 15_000_000 + idx * 1000);
    }

    // Duplicate seq within 60s must be ignored, while boot seq=1 is kept
    const dup = store.recordPowerEvent('D93468', {
      evt: 'pwr',
      seq: 7,
      why: 'probe',
      pct: 80,
      mv: 4180,
      st: 3,
    }, t0 + 15_010_000);
    assert.equal(dup, null);

    await store.flush(t0 + 16_000_000);
    const rep = store.getReport('D93468');
    assert.equal(rep.counters.boots, 1);
    assert.equal(rep.counters.pwrLost, 1);
    assert.equal(rep.counters.dischargeCycles, 2);
    assert.equal(rep.counters.chargeCycles, 1);
    assert.equal(rep.drainRates.screenOn.pctPerHour, 25);
    assert.equal(rep.drainRates.screenOn.estHours, 4);
    assert.equal(rep.drainRates.aod.pctPerHour, 10);
    assert.equal(rep.drainRates.aod.estHours, 10);
    assert.equal(rep.drainRates.overall.pctPerHour, 15);
    assert.equal(rep.chargeProfile.avgPctPerHour, 30);
    assert.equal(rep.chargeProfile.minVbusMv, 4420);
    assert.equal(rep.chargeProfile.vbusLimRatio, 0.5);
    assert.equal(rep.calibration.chgMohm, 800);
    assert.equal(rep.calibration.mohmDelta, 180);
    assert.equal(rep.calibration.avgGaugeBiasPct, 5);
    assert.ok(rep.alerts.some((a) => a.includes('vsys_uvlo')));
    assert.ok(rep.alerts.some((a) => a.includes('VBUS')));
    assert.ok(rep.alerts.some((a) => a.includes('温控')));
    assert.ok(rep.alerts.some((a) => a.includes('+180 mΩ')));
    // Last probe at 80% in CC stage with avgPctPerHour=30 => (100-80)/30 = 0.7h / 40min
    assert.equal(rep.eta.pct, 80);
    assert.equal(rep.eta.charging, true);
    assert.equal(rep.eta.toFullMinutes, 40);
    assert.equal(rep.eta.toFullHours, 0.7);
    assert.equal(rep.eta.byScenario.screenOn, 3.2);
    assert.equal(rep.eta.byScenario.aod, 8);

    // Newer stat frame on AOD discharge at 60% switches ETA scenario to AOD (60 / 10 = 6h)
    store.recordStat('D93468', {
      evt: 'stat', mv: 3900, pct: 60, chg: 0, stage: 1, tmp: 29, lcd: 1, aod: 1, brt: 40, mhz: 80, rssi: -45,
    }, t0 + 15_500_000);
    const repAod = store.getReport('D93468');
    assert.equal(repAod.eta.charging, false);
    assert.equal(repAod.eta.activeScenario, 'aod');
    assert.equal(repAod.eta.remainHours, 6);
    assert.equal(repAod.eta.toFullMinutes, null);
  });

  it('closes a discharge session on reboot so cutoff runs reach the drain model', async () => {
    // 2026-11-24: the device ran flat and rebooted on the charger without ever
    // emitting usb_in, so the 3h28m 72%->5% discharge never landed in `cycles`
    // and the drain model stayed "样本不足" all day.
    const home = await mkdtemp(join(tmpdir(), 'nova-pwr-bootclose-'));
    const store = createPowerStore({ dshHome: home, statPersistMs: 60 * 60 * 1000 });
    const t0 = 1_795_497_000_000;
    store.recordPowerEvent('D93468', {
      evt: 'pwr', why: 'usb_out', seq: 1, pct: 72, mv: 4185, st: 1, usb: 0,
      wm: { dur: 0, sp: 72, vmin: 4185, vmax: 4185, vsmin: 4401, vbmin: 0, tmax: 37, scrS: 0 },
    }, t0);
    store.recordPowerEvent('D93468', {
      evt: 'pwr', why: 'low_bat', seq: 5, pct: 5, mv: 3479, st: 1, usb: 0,
      wm: { dur: 12506, sp: 72, vmin: 3474, vmax: 4185, vsmin: 3447, vbmin: 0, tmax: 37, scrS: 579 },
    }, t0 + 12_000_000);
    assert.equal(store.getReport('D93468').counters.dischargeCycles, 0);

    store.recordPowerEvent('D93468', {
      evt: 'pwr', why: 'boot', seq: 1, pct: 4, raw: 45, gauge: 5, st: 3, mv: 3801, rmv: 3647,
      vb: 5180, vs: 4014, usb: 1, seed: 0, lost: 0, mah: 300, mohm: 520, onSrc: 4, offSrc: 8,
    }, t0 + 12_100_000);

    const report = store.getReport('D93468');
    assert.equal(report.counters.dischargeCycles, 1);
    assert.equal(report.activeWatermark, null);
    const cycle = report.recentCycles.at(-1);
    assert.equal(cycle.kind, 'discharge');
    assert.equal(cycle.durSec, 12506);
    assert.equal(cycle.startPct, 72);
    assert.equal(cycle.endPct, 0);
    assert.equal(cycle.closedBy, 'boot');
  });
});
