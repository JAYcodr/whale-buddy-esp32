/**
 * 设备电源事件有界存储与分析器（host-plugin/lib/power.js）。
 *
 * 设计目标：
 *   1. 严格防爆盘：`power-events.jsonl` + `.1.jsonl` 单文件上限 256 KiB（总计 <= 512 KiB），
 *      只落盘设备关键帧事件（`evt:"pwr"`），不把每 30 秒一次的 `stat` 心跳写入流水日志。
 *   2. 聚合画像常驻：`power-summary.json` 按设备保留有界历史（最近 60 个充放周期、30 次启动、
 *      40 次 OCV 静置探针），用于续航分摊、线材压降、热节流与内阻漂移分析。
 *   3. 零 Race / 确定性落盘：初始化同步加载已有 summary（< 30 KB），所有异步落盘串行队列化，
 *      并使用临时文件 + rename 原子替换 summary。
 */

import fs, { promises as fsp } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

export const BATT_STAGE_NAMES = [
  'NONE',
  'DISCHARGE',
  'PRE',
  'CC',
  'CV',
  'DONE',
  'LIMITED',
  'USB_IDLE',
];

const PWRON_SOURCE_BITS = [
  [0x01, 'pwron_low'],
  [0x02, 'irq_low'],
  [0x04, 'vbus_insert'],
  [0x08, 'bat_normal'],
  [0x10, 'bat_insert'],
  [0x20, 'pwron_en'],
];

const PWROFF_SOURCE_BITS = [
  [0x01, 'pwron_off'],
  [0x02, 'sw_off'],
  [0x04, 'pwron_always_low'],
  [0x08, 'vsys_uvlo'],
  [0x10, 'vbus_ov'],
  [0x20, 'dcdc_uv'],
  [0x40, 'dcdc_ov'],
  [0x80, 'over_temp'],
];

const UNEXPECTED_OFF_TAGS = new Set(['vsys_uvlo', 'vbus_ov', 'dcdc_uv', 'dcdc_ov', 'over_temp']);

const SUMMARY_VERSION = 1;
const DEFAULT_MAX_LOG_BYTES = 256 * 1024;
const DEFAULT_MAX_CYCLES = 60;
const DEFAULT_MAX_BOOTS = 30;
const DEFAULT_MAX_PROBES = 40;
const DEFAULT_STAT_PERSIST_MS = 5 * 60 * 1000;

export function decodeBitMask(mask, table) {
  const n = Number(mask);
  if (!Number.isFinite(n) || n <= 0) return [];
  const tags = [];
  for (const [bit, label] of table) {
    if ((n & bit) !== 0) tags.push(label);
  }
  return tags;
}

export function decodePowerOnSource(mask) {
  return decodeBitMask(mask, PWRON_SOURCE_BITS);
}

export function decodePowerOffSource(mask) {
  return decodeBitMask(mask, PWROFF_SOURCE_BITS);
}

export function stageNameOf(code) {
  const idx = Number(code);
  return Number.isInteger(idx) && idx >= 0 && idx < BATT_STAGE_NAMES.length
    ? BATT_STAGE_NAMES[idx]
    : 'UNKNOWN';
}

function emptyDeviceProfile() {
  return {
    lastSeenAt: 0,
    lastSeq: 0,
    lastWhy: '',
    lastEvent: null,
    lastStat: null,
    activeWatermark: null,
    counters: {
      events: 0,
      boots: 0,
      pwrLost: 0,
      chargeCycles: 0,
      dischargeCycles: 0,
      lowBatCount: 0,
      thermEvents: 0,
      vbusLimSecTotal: 0,
      thermRegSecTotal: 0,
      tempCapSecTotal: 0,
      screenOnSecTotal: 0,
      aodSecTotal: 0,
      onSrcCounts: {},
      offSrcCounts: {},
    },
    boots: [],
    probes: [],
    cycles: [],
  };
}

function pushBounded(list, item, maxLen) {
  list.push(item);
  if (list.length > maxLen) {
    list.splice(0, list.length - maxLen);
  }
}

function bumpTagCounts(map, tags) {
  for (const tag of tags) {
    map[tag] = (map[tag] || 0) + 1;
  }
}

function toWatermarkObj(raw) {
  if (!raw || typeof raw !== 'object') return null;
  return {
    durSec: Number(raw.dur) || 0,
    startPct: Number(raw.sp) || 0,
    vbatMinMv: Number(raw.vmin) || 0,
    vbatMaxMv: Number(raw.vmax) || 0,
    vsysMinMv: Number(raw.vsmin) || 0,
    vbusMinMv: Number(raw.vbmin) || 0,
    tmpMaxC: raw.tmax != null ? Number(raw.tmax) : null,
    vbusLimSec: Number(raw.vlimS) || 0,
    thermRegSec: Number(raw.tregS) || 0,
    tempCapSec: Number(raw.tcapS) || 0,
    screenOnSec: Number(raw.scrS) || 0,
    aodSec: Number(raw.aodS) || 0,
  };
}

function safeFileSize(path) {
  try {
    return fs.statSync(path).size;
  } catch {
    return 0;
  }
}

function weightedDrainRate(cycles) {
  if (cycles.length === 0) return null;
  let totalDelta = 0;
  let totalHours = 0;
  for (const c of cycles) {
    totalDelta += c.deltaPct;
    totalHours += c.durSec / 3600;
  }
  if (totalHours <= 0 || totalDelta <= 0) return null;
  const pctPerHour = Math.round((totalDelta / totalHours) * 100) / 100;
  return {
    cycles: cycles.length,
    totalHours: Math.round(totalHours * 100) / 100,
    totalDeltaPct: totalDelta,
    pctPerHour,
    estHours: pctPerHour > 0 ? Math.round((100 / pctPerHour) * 10) / 10 : null,
  };
}

function summarizeDrainRates(cycles) {
  const valid = cycles.filter(
    (c) => c.kind === 'discharge' && c.durSec >= 600 && c.deltaPct >= 2,
  );
  const screenHeavy = [];
  const aodDominant = [];
  const idleDominant = [];

  for (const c of valid) {
    const scrRatio = c.screenOnSec / c.durSec;
    const aodRatio = c.aodSec / c.durSec;
    if (scrRatio >= 0.5) screenHeavy.push(c);
    else if (aodRatio >= 0.5) aodDominant.push(c);
    else if (scrRatio + aodRatio < 0.2) idleDominant.push(c);
  }

  return {
    overall: weightedDrainRate(valid),
    screenOn: weightedDrainRate(screenHeavy),
    aod: weightedDrainRate(aodDominant),
    idle: weightedDrainRate(idleDominant),
  };
}

function summarizeChargeProfile(cycles) {
  const valid = cycles.filter((c) => c.kind === 'charge' && c.durSec >= 300);
  if (valid.length === 0) return null;

  let totalSec = 0;
  let totalGain = 0;
  let totalVlimSec = 0;
  let totalThermSec = 0;
  let minVbusMv = 0;
  let minVsysMv = 0;
  let maxTmpC = null;

  for (const c of valid) {
    totalSec += c.durSec;
    if (c.deltaPct > 0) totalGain += c.deltaPct;
    totalVlimSec += c.vbusLimSec;
    totalThermSec += c.thermRegSec + c.tempCapSec;
    if (c.vbusMinMv > 0 && (minVbusMv === 0 || c.vbusMinMv < minVbusMv)) {
      minVbusMv = c.vbusMinMv;
    }
    if (c.vsysMinMv > 0 && (minVsysMv === 0 || c.vsysMinMv < minVsysMv)) {
      minVsysMv = c.vsysMinMv;
    }
    if (c.tmpMaxC != null && (maxTmpC === null || c.tmpMaxC > maxTmpC)) {
      maxTmpC = c.tmpMaxC;
    }
  }

  const totalHours = totalSec / 3600;
  return {
    cycles: valid.length,
    totalHours: Math.round(totalHours * 100) / 100,
    avgPctPerHour: totalHours > 0 ? Math.round((totalGain / totalHours) * 10) / 10 : null,
    vbusLimRatio: totalSec > 0 ? Math.round((totalVlimSec / totalSec) * 1000) / 1000 : 0,
    thermalSec: totalThermSec,
    minVbusMv,
    minVsysMv,
    maxTmpC,
  };
}

function summarizeCalibration(dev) {
  const mohmPoints = [];
  for (const b of dev.boots) {
    if (b.mohm > 0) mohmPoints.push({ ts: b.ts, mohm: b.mohm, src: 'boot' });
  }
  for (const p of dev.probes) {
    if (p.mohm > 0) mohmPoints.push({ ts: p.ts, mohm: p.mohm, src: 'probe' });
  }
  mohmPoints.sort((a, b) => a.ts - b.ts);

  const firstMohm = mohmPoints.length > 0 ? mohmPoints[0].mohm : null;
  const latestMohm =
    dev.lastEvent?.mohm ?? (mohmPoints.length > 0 ? mohmPoints[mohmPoints.length - 1].mohm : null);
  const mohmDelta =
    firstMohm != null && latestMohm != null && mohmPoints.length >= 2 ? latestMohm - firstMohm : 0;

  let biasSum = 0;
  let biasCnt = 0;
  for (const p of dev.probes) {
    if (p.bias != null) {
      biasSum += p.bias;
      biasCnt += 1;
    }
  }

  return {
    chgMohm: latestMohm,
    designMah: dev.lastEvent?.mah ?? null,
    mohmDelta,
    probeCount: dev.probes.length,
    avgGaugeBiasPct: biasCnt > 0 ? Math.round((biasSum / biasCnt) * 10) / 10 : null,
    latestProbe: dev.probes.length > 0 ? dev.probes[dev.probes.length - 1] : null,
  };
}

function buildAlerts(dev, chargeProfile, calibration) {
  const alerts = [];
  const lastBoot = dev.boots.length > 0 ? dev.boots[dev.boots.length - 1] : null;
  if (lastBoot) {
    const badOff = lastBoot.offTags.filter((t) => UNEXPECTED_OFF_TAGS.has(t));
    if (badOff.length > 0) {
      alerts.push(`最近一次异常关机源: ${badOff.join(', ')}`);
    } else if (lastBoot.lost) {
      alerts.push('最近一次开机前 PMU 发生掉电复位');
    }
  }
  if (chargeProfile) {
    if (
      (chargeProfile.minVbusMv > 0 && chargeProfile.minVbusMv < 4550) ||
      chargeProfile.vbusLimRatio > 0.25
    ) {
      alerts.push(
        `供电口或线材压降明显（VBUS 最低 ${chargeProfile.minVbusMv || '?'} mV，限流占比 ${Math.round(chargeProfile.vbusLimRatio * 100)}%）`,
      );
    }
    if (chargeProfile.thermalSec > 120 || (chargeProfile.maxTmpC != null && chargeProfile.maxTmpC >= 46)) {
      alerts.push(
        `充电温控触发较多（累计 ${chargeProfile.thermalSec}s，最高 ${chargeProfile.maxTmpC ?? '?'}°C）`,
      );
    }
  }
  if (calibration.mohmDelta >= 150 && calibration.probeCount >= 3) {
    alerts.push(`电池通路等效内阻累计上升 +${calibration.mohmDelta} mΩ`);
  }
  return alerts;
}

function remainHoursFor(pct, rate) {
  if (!rate || !(rate.pctPerHour > 0) || pct == null || pct < 0) return null;
  if (pct === 0) return 0;
  return Math.round((pct / rate.pctPerHour) * 10) / 10;
}

export function estimatePowerEta(dev, drainRates, chargeProfile) {
  const evt = dev?.lastEvent || null;
  const st = dev?.lastStat || null;
  if (!evt && !st) return null;

  const useStat = Boolean(st && (!evt || (Number(st.ts) || 0) >= (Number(evt.ts) || 0)));
  const latest = useStat ? st : evt;
  const pct = Number.isFinite(Number(latest.pct)) ? Number(latest.pct) : null;
  const stageName = latest.stageName || stageNameOf(latest.st ?? latest.stage);
  const isChargingStage = stageName === 'PRE' || stageName === 'CC' || stageName === 'CV';
  const isUsbIdle = stageName === 'USB_IDLE';
  const isDone = stageName === 'DONE' || stageName === 'LIMITED';
  // USB_IDLE（已接电但未充）直接短路 charging，避免 chg 位异常置 1 时误入充电 ETA。
  const charging = !isUsbIdle && !isDone && (Boolean(latest.chg) || isChargingStage);
  const aod = Boolean(st?.aod);
  const lcd = st ? Boolean(st.lcd) : true;

  const byScenario = {
    overall: remainHoursFor(pct, drainRates?.overall),
    screenOn: remainHoursFor(pct, drainRates?.screenOn),
    aod: remainHoursFor(pct, drainRates?.aod),
    idle: remainHoursFor(pct, drainRates?.idle),
  };

  let activeScenario = 'unknown';
  let activeRate = null;
  if (charging) {
    activeScenario = 'charging';
    activeRate = drainRates?.overall || null;
  } else if (isUsbIdle) {
    activeScenario = 'usbIdle';
  } else if (aod) {
    activeScenario = drainRates?.aod ? 'aod' : (drainRates?.overall ? 'overall' : 'unknown');
    activeRate = drainRates?.aod || drainRates?.overall || null;
  } else if (lcd) {
    activeScenario = drainRates?.screenOn ? 'screenOn' : (drainRates?.overall ? 'overall' : 'unknown');
    activeRate = drainRates?.screenOn || drainRates?.overall || null;
  } else {
    activeScenario = drainRates?.idle ? 'idle' : (drainRates?.overall ? 'overall' : 'unknown');
    activeRate = drainRates?.idle || drainRates?.overall || null;
  }

  let toFullHours = null;
  let toFullMinutes = null;
  const chargePctPerHour = chargeProfile?.avgPctPerHour > 0 ? chargeProfile.avgPctPerHour : null;
  if (charging && pct != null && pct < 100 && chargePctPerHour != null) {
    const rawHours = (100 - pct) / chargePctPerHour;
    toFullHours = Math.round(rawHours * 10) / 10;
    toFullMinutes = Math.max(1, Math.round(rawHours * 60));
  }

  return {
    pct,
    charging,
    usbIdle: isUsbIdle,
    stageName,
    activeScenario,
    remainHours: remainHoursFor(pct, activeRate),
    toFullHours,
    toFullMinutes,
    chargePctPerHour,
    byScenario,
  };
}

/**
 * Format ETA summary text for CLI display (`已接电未充` / charge ETA / drain ETA).
 * @param {ReturnType<typeof estimatePowerEta>} eta
 * @returns {string | null}
 */
export function formatPowerEta(eta) {
  if (!eta) return null;
  if (eta.usbIdle) {
    return eta.pct != null ? `当前电量(${eta.pct}%)已接电未充` : '已接电未充';
  }
  const parts = [];
  if (eta.charging && eta.toFullMinutes != null) {
    parts.push(`预计充满还需 ${eta.toFullMinutes} 分钟 (${eta.toFullHours}h)`);
  }
  if (eta.remainHours != null) {
    parts.push(`当前电量(${eta.pct}%)按 ${eta.activeScenario} 预计可用 ${eta.remainHours}h`);
  }
  return parts.length > 0 ? parts.join(' | ') : null;
}

/**
 * @param {object} [options]
 * @param {string} [options.dshHome]
 * @param {string} [options.dataDir]
 * @param {string} [options.logPath]
 * @param {string} [options.archivePath]
 * @param {string} [options.summaryPath]
 * @param {number} [options.maxLogBytes]
 * @param {number} [options.maxCycles]
 * @param {number} [options.maxBoots]
 * @param {number} [options.maxProbes]
 * @param {number} [options.statPersistMs]
 * @param {boolean} [options.enabled]
 * @param {(msg: string) => void} [options.log]
 */
export function createPowerStore(options = {}) {
  const log = options.log || (() => {});
  const warn = (msg) => (typeof log.warn === 'function' ? log.warn(msg) : log(msg));
  const enabled = options.enabled !== false;

  const dshHome = options.dshHome || process.env.DSH_HOME || join(homedir(), '.dsh');
  const dataDir = options.dataDir || join(dshHome, 'nova-buddy');
  const logPath = options.logPath || join(dataDir, 'power-events.jsonl');
  const archivePath = options.archivePath || join(dataDir, 'power-events.1.jsonl');
  const summaryPath = options.summaryPath || join(dataDir, 'power-summary.json');

  const maxLogBytes = Math.max(4096, Number(options.maxLogBytes ?? DEFAULT_MAX_LOG_BYTES));
  const maxCycles = Math.max(5, Number(options.maxCycles ?? DEFAULT_MAX_CYCLES));
  const maxBoots = Math.max(5, Number(options.maxBoots ?? DEFAULT_MAX_BOOTS));
  const maxProbes = Math.max(5, Number(options.maxProbes ?? DEFAULT_MAX_PROBES));
  const statPersistMs = Math.max(0, Number(options.statPersistMs ?? DEFAULT_STAT_PERSIST_MS));

  const state = {
    version: SUMMARY_VERSION,
    updatedAt: 0,
    devices: {},
  };

  let logBytes = 0;
  let summaryDirty = false;
  let statDirty = false;
  let lastSummaryFlushAt = 0;
  let dirReady = false;
  let writeChain = Promise.resolve();

  if (enabled) {
    try {
      const parsed = JSON.parse(fs.readFileSync(summaryPath, 'utf8'));
      if (parsed?.version === SUMMARY_VERSION && parsed.devices && typeof parsed.devices === 'object') {
        state.updatedAt = Number(parsed.updatedAt) || 0;
        for (const [id, profile] of Object.entries(parsed.devices)) {
          const base = emptyDeviceProfile();
          state.devices[id] = {
            ...base,
            ...profile,
            counters: { ...base.counters, ...(profile?.counters || {}) },
            boots: Array.isArray(profile?.boots) ? profile.boots.slice(-maxBoots) : [],
            probes: Array.isArray(profile?.probes) ? profile.probes.slice(-maxProbes) : [],
            cycles: Array.isArray(profile?.cycles) ? profile.cycles.slice(-maxCycles) : [],
          };
        }
      }
    } catch {
      /* 首次启动或摘要损坏时从空白开始 */
    }
    logBytes = safeFileSize(logPath);
  }

  function ensureDevice(id) {
    if (!state.devices[id]) state.devices[id] = emptyDeviceProfile();
    return state.devices[id];
  }

  function enqueueWrite(task) {
    if (!enabled) return;
    writeChain = writeChain.then(task).catch((err) => {
      warn(`nova-buddy: power store write failed ${err?.message || err}`);
    });
  }

  async function ensureDir() {
    if (dirReady) return;
    await fsp.mkdir(dataDir, { recursive: true });
    dirReady = true;
  }

  function scheduleLogLine(entry) {
    if (!enabled) return;
    const line = `${JSON.stringify(entry)}\n`;
    const bytes = Buffer.byteLength(line, 'utf8');
    enqueueWrite(async () => {
      await ensureDir();
      if (logBytes + bytes > maxLogBytes && logBytes > 0) {
        try {
          await fsp.rename(logPath, archivePath);
        } catch (err) {
          if (err?.code !== 'ENOENT') throw err;
        }
        logBytes = 0;
      }
      await fsp.appendFile(logPath, line, 'utf8');
      logBytes += bytes;
    });
  }

  function scheduleSummaryWrite(nowMs = Date.now()) {
    if (!enabled) return;
    summaryDirty = false;
    statDirty = false;
    lastSummaryFlushAt = nowMs;
    state.updatedAt = nowMs;
    const snapshot = JSON.stringify(state);
    enqueueWrite(async () => {
      await ensureDir();
      const tmpPath = `${summaryPath}.tmp`;
      await fsp.writeFile(tmpPath, snapshot, 'utf8');
      await fsp.rename(tmpPath, summaryPath);
    });
  }

  function recordPowerEvent(id, data, nowMs = Date.now()) {
    if (!id || data?.evt !== 'pwr') return null;
    const dev = ensureDevice(id);
    const why = String(data.why || 'none');
    const seq = Number(data.seq) || 0;

    // QoS1 断线重连可能重投同一序号；`boot` 序号会重置为 1，不在此列。
    if (why !== 'boot' && seq > 0 && seq === dev.lastSeq && nowMs - dev.lastSeenAt < 60000) {
      return null;
    }

    const onTags = why === 'boot' ? decodePowerOnSource(data.onSrc) : undefined;
    const offTags = why === 'boot' ? decodePowerOffSource(data.offSrc) : undefined;
    const wm = toWatermarkObj(data.wm);

    const record = {
      ts: nowMs,
      dev: id,
      ...data,
      stageName: stageNameOf(data.st),
      ...(data.pst != null ? { prevStageName: stageNameOf(data.pst) } : {}),
      ...(onTags ? { onTags } : {}),
      ...(offTags ? { offTags } : {}),
    };

    dev.lastSeenAt = nowMs;
    dev.lastSeq = seq;
    dev.lastWhy = why;
    dev.lastEvent = {
      ts: nowMs,
      why,
      seq,
      pct: Number(data.pct) || 0,
      raw: Number(data.raw) || 0,
      gauge: data.gauge != null ? Number(data.gauge) : -1,
      stage: Number(data.st) || 0,
      stageName: stageNameOf(data.st),
      mv: Number(data.mv) || 0,
      cmv: Number(data.cmv) || 0,
      rmv: Number(data.rmv) || 0,
      vb: Number(data.vb) || 0,
      vs: Number(data.vs) || 0,
      ma: Number(data.ma) || 0,
      ema: Number(data.ema) || 0,
      tmp: Number(data.tmp) || 0,
      usb: Boolean(data.usb),
      chg: Boolean(data.chg),
      lim: Boolean(data.lim),
      tcap: Boolean(data.tcap),
      treg: Boolean(data.treg),
      vlim: Boolean(data.vlim),
      mohm: data.mohm != null ? Number(data.mohm) : null,
      mah: data.mah != null ? Number(data.mah) : null,
    };

    dev.counters.events += 1;
    if (wm) {
      dev.activeWatermark = (why === 'usb_in' || why === 'usb_out') ? null : wm;
    }

    if (why === 'boot') {
      // A device that runs itself flat and reboots on the charger never emits
      // the `usb_in` that used to close a discharge session: the boot frame
      // replaces it (2026-11-24 lost a 3h28m 72%->5% run exactly this way, so
      // every real discharge cycle since has been missing from `cycles` and the
      // drain model stayed "样本不足"). Close it here from the watermark that was
      // still open when the device went down. vbusMinMv>0 means the session was
      // a charge session; only battery-only watermarks become discharge cycles.
      const wm = dev.activeWatermark;
      if (wm && wm.durSec >= 60 && Number(wm.vbusMinMv) === 0) {
        const endPct = data.seed != null && Number(data.seed) >= 0
          ? Number(data.seed)
          : (Number(data.pct) || 0);
        pushBounded(dev.cycles, {
          ts: nowMs,
          kind: 'discharge',
          durSec: wm.durSec,
          startPct: wm.startPct,
          endPct,
          deltaPct: wm.startPct - endPct,
          vbatMinMv: wm.vbatMinMv,
          vbatMaxMv: wm.vbatMaxMv,
          vsysMinMv: wm.vsysMinMv,
          vbusMinMv: wm.vbusMinMv,
          tmpMaxC: wm.tmpMaxC,
          vbusLimSec: wm.vbusLimSec,
          thermRegSec: wm.thermRegSec,
          tempCapSec: wm.tempCapSec,
          screenOnSec: wm.screenOnSec,
          aodSec: wm.aodSec,
          mohm: data.mohm != null ? Number(data.mohm) : null,
          mah: data.mah != null ? Number(data.mah) : null,
          closedBy: 'boot',
        }, maxCycles);
        dev.counters.dischargeCycles += 1;
        dev.counters.vbusLimSecTotal += wm.vbusLimSec;
        dev.counters.thermRegSecTotal += wm.thermRegSec;
        dev.counters.tempCapSecTotal += wm.tempCapSec;
        dev.counters.screenOnSecTotal += wm.screenOnSec;
        dev.counters.aodSecTotal += wm.aodSec;
        dev.activeWatermark = null;
      }
      dev.counters.boots += 1;
      if (data.lost) dev.counters.pwrLost += 1;
      bumpTagCounts(dev.counters.onSrcCounts, onTags || []);
      bumpTagCounts(dev.counters.offSrcCounts, offTags || []);
      pushBounded(
        dev.boots,
        {
          ts: nowMs,
          onSrc: Number(data.onSrc) || 0,
          onTags: onTags || [],
          offSrc: Number(data.offSrc) || 0,
          offTags: offTags || [],
          bootMv: Number(data.rmv || data.mv) || 0,
          seed: data.seed != null ? Number(data.seed) : -1,
          lost: Boolean(data.lost),
          mohm: data.mohm != null ? Number(data.mohm) : null,
          mah: data.mah != null ? Number(data.mah) : null,
          vbusMv: Number(data.vb) || 0,
          vsysMv: Number(data.vs) || 0,
        },
        maxBoots,
      );
    } else if (why === 'probe') {
      const gauge = data.gauge != null ? Number(data.gauge) : -1;
      const pct = Number(data.pct) || 0;
      pushBounded(
        dev.probes,
        {
          ts: nowMs,
          pct,
          raw: Number(data.raw) || 0,
          gauge,
          bias: gauge >= 0 ? pct - gauge : null,
          mv: Number(data.mv) || 0,
          rmv: Number(data.rmv) || 0,
          ema: Number(data.ema) || 0,
          mohm: data.mohm != null ? Number(data.mohm) : null,
          tmp: Number(data.tmp) || 0,
        },
        maxProbes,
      );
    } else if ((why === 'usb_in' || why === 'usb_out') && wm) {
      const isDischargeClose = why === 'usb_in';
      const endPct = Number(data.pct) || 0;
      const cycle = {
        ts: nowMs,
        kind: isDischargeClose ? 'discharge' : 'charge',        durSec: wm.durSec,
        startPct: wm.startPct,
        endPct,
        deltaPct: isDischargeClose ? wm.startPct - endPct : endPct - wm.startPct,
        vbatMinMv: wm.vbatMinMv,
        vbatMaxMv: wm.vbatMaxMv,
        vsysMinMv: wm.vsysMinMv,
        vbusMinMv: wm.vbusMinMv,
        tmpMaxC: wm.tmpMaxC,
        vbusLimSec: wm.vbusLimSec,
        thermRegSec: wm.thermRegSec,
        tempCapSec: wm.tempCapSec,
        screenOnSec: wm.screenOnSec,
        aodSec: wm.aodSec,
        mohm: data.mohm != null ? Number(data.mohm) : null,
        mah: data.mah != null ? Number(data.mah) : null,
      };
      pushBounded(dev.cycles, cycle, maxCycles);
      if (isDischargeClose) dev.counters.dischargeCycles += 1;
      else dev.counters.chargeCycles += 1;
      dev.counters.vbusLimSecTotal += wm.vbusLimSec;
      dev.counters.thermRegSecTotal += wm.thermRegSec;
      dev.counters.tempCapSecTotal += wm.tempCapSec;
      dev.counters.screenOnSecTotal += wm.screenOnSec;
      dev.counters.aodSecTotal += wm.aodSec;
    } else if (why === 'therm') {
      dev.counters.thermEvents += 1;
    } else if (why === 'low_bat') {
      dev.counters.lowBatCount += 1;
    }

    scheduleLogLine(record);
    summaryDirty = true;
    scheduleSummaryWrite(nowMs);
    return record;
  }

  function recordStat(id, data, nowMs = Date.now()) {
    if (!id || data?.evt !== 'stat') return;
    const dev = ensureDevice(id);
    dev.lastSeenAt = nowMs;
    dev.lastStat = {
      ts: nowMs,
      mv: Number(data.mv) || 0,
      pct: Number(data.pct) || 0,
      chg: Boolean(data.chg),
      stage: Number(data.stage) || 0,
      stageName: stageNameOf(data.stage),
      tmp: Number(data.tmp) || 0,
      lcd: data.lcd !== 0,
      aod: Boolean(data.aod),
      brt: Number(data.brt) || 0,
      mhz: Number(data.mhz) || 0,
      rssi: Number(data.rssi) || 0,
    };
    statDirty = true;
    if (nowMs - lastSummaryFlushAt >= statPersistMs) {
      scheduleSummaryWrite(nowMs);
    }
  }

  function storageStats() {
    const activeBytes = safeFileSize(logPath);
    const archiveBytes = safeFileSize(archivePath);
    const summaryBytes = safeFileSize(summaryPath);
    return {
      dataDir,
      logBytes: activeBytes,
      archiveBytes,
      summaryBytes,
      totalBytes: activeBytes + archiveBytes + summaryBytes,
      maxLogTotalBytes: maxLogBytes * 2,
    };
  }

  function getReport(id) {
    const targetId = id || Object.keys(state.devices)[0];
    if (!targetId || !state.devices[targetId]) return null;
    const dev = state.devices[targetId];
    const drainRates = summarizeDrainRates(dev.cycles);
    const chargeProfile = summarizeChargeProfile(dev.cycles);
    const calibration = summarizeCalibration(dev);
    const alerts = buildAlerts(dev, chargeProfile, calibration);
    const eta = estimatePowerEta(dev, drainRates, chargeProfile);
    return {
      deviceId: targetId,
      updatedAt: state.updatedAt || dev.lastSeenAt,
      lastBoot: dev.boots.length > 0 ? dev.boots[dev.boots.length - 1] : null,
      lastEvent: dev.lastEvent,
      lastStat: dev.lastStat,
      activeWatermark: dev.activeWatermark,
      counters: dev.counters,
      calibration,
      drainRates,
      chargeProfile,
      eta,
      recentCycles: dev.cycles.slice(-10),
      alerts,
      storage: storageStats(),
    };
  }

  function flush(nowMs = Date.now()) {
    if (summaryDirty || statDirty) {
      scheduleSummaryWrite(nowMs);
    }
    return writeChain;
  }

  return {
    recordPowerEvent,
    recordStat,
    getReport,
    listDeviceIds: () => Object.keys(state.devices),
    getSummary: () => state,
    storageStats,
    flush,
  };
}
