#!/usr/bin/env node
// Nova Buddy 功耗/渲染采样器：订阅 MQTT 上的 stat/hello，落 CSV 并出汇总。
//
// 用法：
//   node tools/power_probe.mjs --id D93468 --minutes 30
//   node tools/power_probe.mjs --id D93468 --minutes 20 --tag aod-off
//   node tools/power_probe.mjs --id D93468 --watch          # 只打印，不写文件
//
// 为什么是 MQTT 而不是串口：设备在桌上连着 WiFi，插着 USB 也未必有数据口。
// 采的是 up（30s 一条，可用 {"cmd":"statrate","ms":5000} 临时改密）。
//
// 口径说明（重要）：
// - AXP2101 没有电池电流 ADC、库仑计未校准，所以电池侧只能看 mV 斜率与 pct/h。
// - 充电段（stage=PRE/CC/CV/DONE/USB_IDLE）的电压不是放电曲线，脚本会自动跳过斜率，
//   只报 CPU/推送/温度——不拿充电数据冒充续航。

import { createRequire } from 'node:module';
import { mkdirSync, appendFileSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(new URL('../host-plugin/', import.meta.url));
const mqtt = require('mqtt');

const args = process.argv.slice(2);
const arg = (name, def = '') => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : def;
};
const has = (name) => args.includes(`--${name}`);

const deviceId = arg('id', process.env.NOVA_DEVICE_ID || 'D93468');
const brokerUrl = arg('url', process.env.NOVA_MQTT_URL || 'mqtt://192.168.31.15:1883');
const minutes = Number(arg('minutes', '30')) || 30;
const watchOnly = has('watch');
const tag = arg('tag', '');
const outFile = arg('out', '') || join(
  homedir(), '.dsh', 'nova-buddy',
  `power-${deviceId}-${new Date().toISOString().replace(/[:.]/g, '-')}${tag ? `-${tag}` : ''}.csv`,
);

const COLUMNS = [
  'ts', 'iso', 'fps', 'drawFps', 'pushUs', 'idle', 'draws', 'heap', 'psram', 'mhz',
  'rssi', 'mv', 'pct', 'stage', 'chg', 'tmp', 'pwr', 'brt', 'lcd', 'aod', 'cpuUi', 'cpuDec',
];

const samples = [];
let statCount = 0;
let stateCount = 0;
let startedAt = Date.now();

const num = (v, def = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : def);

function record(msg) {
  const ts = Date.now();
  const row = {
    ts,
    iso: new Date(ts).toISOString(),
    fps: num(msg.fps),
    drawFps: num(msg.drawFps),
    pushUs: num(msg.pushUs),
    idle: num(msg.idle),
    draws: num(msg.draws),
    heap: num(msg.heap),
    psram: num(msg.psram),
    mhz: num(msg.mhz),
    rssi: num(msg.rssi),
    mv: num(msg.mv),
    pct: num(msg.pct),
    stage: num(msg.stage),
    chg: num(msg.chg),
    tmp: num(msg.tmp),
    pwr: num(msg.pwr),
    brt: num(msg.brt),
    lcd: num(msg.lcd),
    aod: num(msg.aod),
    cpuUi: num(msg.cpu?.ui),
    cpuDec: num(msg.cpu?.dec),
  };
  samples.push(row);
  if (watchOnly) {
    console.log(
      `${row.iso.slice(11, 19)} fps=${row.fps.toFixed(1)} draw=${row.drawFps.toFixed(1)} ` +
      `push=${(row.pushUs / 1000).toFixed(1)}ms idle=${(row.idle / 1000).toFixed(0)}s ` +
      `mhz=${row.mhz} brt=${row.brt} lcd=${row.lcd} aod=${row.aod} ` +
      `cpu=${row.cpuUi}/${row.cpuDec}% pwr=${row.pwr} ${row.pct}% ${row.mv}mV stage=${row.stage}`,
    );
    return;
  }
  appendFileSync(outFile, `${COLUMNS.map((c) => row[c]).join(',')}\n`);
}

function mean(xs) {
  return xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : 0;
}

function slopePerHour(rows, pick) {
  if (rows.length < 6) return null;
  const t0 = rows[0].ts;
  const xs = rows.map((r) => (r.ts - t0) / 3600000);
  const ys = rows.map(pick);
  const span = xs[xs.length - 1];
  if (span < 0.08) return null; // 少于 ~5 分钟不给斜率
  const mx = mean(xs);
  const my = mean(ys);
  let numr = 0;
  let den = 0;
  for (let i = 0; i < xs.length; i++) {
    numr += (xs[i] - mx) * (ys[i] - my);
    den += (xs[i] - mx) ** 2;
  }
  if (den === 0) return null;
  return numr / den;
}

function summarize() {
  if (!samples.length) {
    console.log('\n没有样本。检查：设备是否在线、topic 是否是 nova/<id>/up、broker 是否可达。');
    return;
  }
  const first = samples[0];
  const last = samples[samples.length - 1];
  const durH = (last.ts - first.ts) / 3600000;
  const draws = last.draws >= first.draws ? last.draws - first.draws : 0;
  const drawPerMin = durH > 0 ? draws / (durH * 60) : 0;
  const lcdPct = (100 * samples.filter((s) => s.lcd === 1).length) / samples.length;
  const aodPct = (100 * samples.filter((s) => s.aod === 1).length) / samples.length;
  const tmp = samples.map((s) => s.tmp).filter((v) => v !== 0);
  const charged = samples.filter((s) => s.stage !== 1); // 1 = DISCHARGE
  const discharging = samples.filter((s) => s.stage === 1);
  const mvSlope = slopePerHour(discharging, (r) => r.mv);
  const pctSlope = slopePerHour(discharging, (r) => r.pct);

  const lines = [];
  lines.push('');
  lines.push(`Nova Buddy 采样汇总  device=${deviceId}  tag=${tag || '-'}`);
  lines.push(`样本 ${samples.length} 条 / ${(durH * 60).toFixed(1)} 分钟  (state 事件 ${stateCount} 条)`);
  lines.push('');
  lines.push('渲染 / CPU');
  lines.push(`  绘制        ${drawPerMin.toFixed(2)} 次/分钟   (累计 ${draws})`);
  lines.push(`  帧率        inst=${mean(samples.map((s) => s.fps)).toFixed(1)}  draw=${mean(samples.map((s) => s.drawFps)).toFixed(1)}`);
  lines.push(`  push        平均 ${(mean(samples.map((s) => s.pushUs)) / 1000).toFixed(2)} ms`);
  lines.push(`  忙时        ui ${mean(samples.map((s) => s.cpuUi)).toFixed(1)}%  dec ${mean(samples.map((s) => s.cpuDec)).toFixed(1)}%`);
  lines.push(`  主频        平均 ${mean(samples.map((s) => s.mhz)).toFixed(0)} MHz`);
  lines.push(`  屏幕        lcd-on ${lcdPct.toFixed(0)}%   AOD ${aodPct.toFixed(0)}%`);
  lines.push(`  温度        ${tmp.length ? `${Math.min(...tmp)}~${Math.max(...tmp)}°C` : 'n/a'}`);
  lines.push(`  内存        heap ${(mean(samples.map((s) => s.heap)) / 1024).toFixed(0)} KB  psram ${(mean(samples.map((s) => s.psram)) / 1048576).toFixed(2)} MB`);
  lines.push('');
  lines.push('电池');
  if (charged.length === samples.length) {
    lines.push('  全程在充电/接 USB：不计算斜率，也不投影续航（AXP2101 没有电流 ADC）。');
  } else if (discharging.length < 6) {
    lines.push(`  放电样本只有 ${discharging.length} 条，不够算斜率。`);
  } else if (mvSlope == null) {
    lines.push('  放电窗口太短（< 5 分钟），不给斜率。');
  } else {
    lines.push(`  放电段 ${discharging.length}/${samples.length} 条`);
    lines.push(`  mV 斜率     ${mvSlope.toFixed(1)} mV/h   ${mvSlope < 0 ? '（在掉电）' : '（未掉电/在充）'}`);
    lines.push(`  pct 斜率    ${(pctSlope ?? 0).toFixed(2)} %/h`);
    if (mvSlope < -1) {
      const remain = (last.mv - 3400) / -mvSlope;
      lines.push(`  投影        从 ${last.mv}mV 到 3.4V 约 ${remain.toFixed(1)} 小时（斜率法，非电流法）`);
    }
  }
  lines.push('');
  console.log(lines.join('\n'));

  if (!watchOnly) {
    const md = outFile.replace(/\.csv$/, '.md');
    writeFileSync(md, `# 采样汇总\n\n\`\`\`\n${lines.join('\n')}\n\`\`\`\n`);
    console.log(`CSV:  ${outFile}`);
    console.log(`汇总: ${md}`);
  }
}

// ── main ────────────────────────────────────────────────────────────
if (!watchOnly) {
  mkdirSync(dirname(outFile), { recursive: true });
  writeFileSync(outFile, `${COLUMNS.join(',')}\n`);
}
console.log(`连接 ${brokerUrl} … 订阅 nova/${deviceId}/{hello,up,state}`);

const client = mqtt.connect(brokerUrl, { clientId: `nova-probe-${process.pid}` });
client.on('connect', () => {
  client.subscribe([`nova/${deviceId}/up`, `nova/${deviceId}/hello`, `nova/${deviceId}/state`], { qos: 0 });
  console.log(`已连接，采样 ${minutes} 分钟（Ctrl-C 提前结束并出汇总）`);
});
client.on('error', (err) => console.error('MQTT 错误:', err.message));
client.on('message', (topic, buf) => {
  let msg;
  try {
    msg = JSON.parse(buf.toString('utf8'));
  } catch {
    return;
  }
  statCount++;
  if (topic.endsWith('/up') && msg.evt !== 'stat') return;
  if (topic.endsWith('/state')) {
    stateCount++;
    return;
  }
  if (topic.endsWith('/up')) record(msg);
});

const done = () => {
  clearInterval(timer);
  summarize();
  client.end(true);
  process.exit(0);
};
process.on('SIGINT', done);
const timer = setTimeout(done, minutes * 60 * 1000);
