#!/usr/bin/env node
import readline from 'node:readline/promises';
import { stdin as input, stdout as output } from 'node:process';
import mqtt from 'mqtt';
import { fetchAllQuota, fetchModelDialRank, saveOpenDesignCookie } from '../lib/fetchers.js';
import { generateChoreography, loadAiConfig, synthesizeSpeechAdpcm } from '../lib/ai.js';
import { createTokenCollector } from '../lib/tokens.js';
import { createPowerStore } from '../lib/power.js';

const args = process.argv.slice(2);
const brokerUrl = process.env.NOVA_MQTT_URL || 'mqtt://192.168.31.15:1883';

function argVal(flag, defVal = '') {
  const i = args.indexOf(flag);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : defVal;
}

async function connectMqtt() {
  const client = mqtt.connect(brokerUrl);
  client.on('error', (e) => console.error('mqtt error', e?.message || e));
  client.on('offline', () => console.warn('mqtt offline'));
  client.on('reconnect', () => console.log('mqtt reconnecting'));
  await new Promise((resolve, reject) => {
    client.once('connect', resolve);
    client.once('error', reject);
  });
  return client;
}

const pubJson = (client, topic, obj, retain = true) =>
  new Promise((res) => client.publish(topic, JSON.stringify(obj), { qos: 1, retain }, res));

async function cmdAuthOpenDesign() {
  let raw = argVal('--cookie', '');
  if (!raw && args.includes('--paste')) {
    const rl = readline.createInterface({ input, output });
    raw = await rl.question('请粘贴 OpenDesign Cookie（或 __Secure-better-auth.session_token 值）：');
    rl.close();
  }
  if (!raw?.trim()) {
    console.error('用法: nova-buddy auth opendesign --paste  或  --cookie "<token>"');
    process.exit(1);
  }
  const saved = saveOpenDesignCookie(raw);
  console.log(`已保存（600 权限）：${saved}`);
}

async function cmdTestChoreo() {
  const devId = argVal('--id', 'D93468');
  const aiCfg = loadAiConfig();
  const choreo = await generateChoreography(
    '触发=手动测试，DSH运行=0，等待=0，会话=2，DeepSeek余额77.08元',
    aiCfg,
    console.log,
  );
  if (!choreo) {
    console.error('LLM 剧本生成失败');
    process.exit(1);
  }
  console.log('Choreo:', JSON.stringify(choreo));
  const client = await connectMqtt();
  await pubJson(client, `nova/${devId}/choreo`, choreo, true);
  client.end();
  console.log(`已推送剧本到 nova/${devId}/choreo`);
}

async function cmdTestTts() {
  const devId = argVal('--id', 'D93468');
  const text = argVal('--text', '主人晚上好呀，诺瓦已经接入小米冰糖语音啦！');
  const voice = argVal('--voice', '');
  const aiCfg = loadAiConfig(voice ? { ttsVoice: voice } : {});
  const adp = await synthesizeSpeechAdpcm(text, { ignoreQuiet: true }, aiCfg);
  if (!adp) {
    console.error('TTS 合成失败');
    process.exit(1);
  }
  console.log(`TTS 合成成功 (${aiCfg.tts.voice}): ${adp.length} B NVAD ADPCM`);
  const client = await connectMqtt();
  const chunkBytes = 1800;
  const total = Math.ceil(adp.length / chunkBytes);
  const clipId = Math.floor(Date.now() / 1000) & 0xFFFF;
  for (let seq = 0; seq < total; seq++) {
    const slice = adp.subarray(seq * chunkBytes, (seq + 1) * chunkBytes);
    await pubJson(
      client,
      `nova/${devId}/tts`,
      { t: 'tts', id: clipId, seq, tot: total, b64: slice.toString('base64') },
      false,
    );
  }
  client.end();
  console.log(`已分片 (${total} 包) 推送到 nova/${devId}/tts`);
}

async function cmdTestPush() {
  const devId = argVal('--id', 'D93468');
  const withApproval = args.includes('--approval');

  console.log('正在抓取实时额度与 ModelDial 榜单...');
  const [quota, rank] = await Promise.all([fetchAllQuota(), fetchModelDialRank(true)]);
  console.log('Quota:', JSON.stringify(quota));
  console.log('Rank rows:', rank?.rows?.length ?? 0);

  const client = await connectMqtt();
  const epoch = Math.floor(Date.now() / 1000);
  const tz = -new Date().getTimezoneOffset() * 60;
  await pubJson(client, `nova/${devId}/time`, { t: 'time', epoch, tz }, true);
  await pubJson(
    client,
    `nova/${devId}/state`,
    {
      t: 'state',
      run: 1,
      wait: withApproval ? 1 : 0,
      sess: 2,
      msg: 'bash: pio run -e waveshare',
      ctx: { pct: 42 },
      model: rank?.rows?.[0]?.m || 'deepseek-v4',
    },
    true,
  );
  await pubJson(client, `nova/${devId}/quota`, quota, true);
  if (rank) await pubJson(client, `nova/${devId}/rank`, rank, true);

  console.log('正在聚合会话日志里的 token 用量...');
  const tokens = await createTokenCollector({ log: (m) => console.log(m) }).refresh();
  if (tokens) {
    console.log(`Tokens: ${tokens.total} / ${tokens.spanDays} 天 / 命中 ${tokens.cachePct}%`);
    await pubJson(client, `nova/${devId}/tokens`, tokens, true);
  }

  if (withApproval) {
    const apId = `ap_test_${Date.now().toString(36)}`;
    client.subscribe(`nova/${devId}/up`, { qos: 1 });
    await pubJson(
      client,
      `nova/${devId}/approval`,
      { t: 'approval', id: apId, tool: 'Bash', reason: 'echo "Nova Buddy E2E Test"', ttl: 15000 },
      false,
    );
    console.log(`已下发测试审批 ${apId}，等待设备屏幕点击或 15s 超时...`);
    await new Promise((resolve) => {
      const t = setTimeout(resolve, 16000);
      client.on('message', (topic, buf) => {
        if (topic === `nova/${devId}/up`) {
          console.log('收到设备上行:', buf.toString('utf8'));
          if (buf.toString('utf8').includes(apId)) {
            clearTimeout(t);
            resolve();
          }
        }
      });
    });
  }

  client.end();
  console.log(`已推送到 nova/${devId}/*`);
}

function formatDrainRate(label, bucket) {
  if (!bucket) return `${label}: 样本不足`;
  return `${label}: ${bucket.pctPerHour}%/h（估算满电 ${bucket.estHours}h，基于 ${bucket.cycles} 个周期 ${bucket.totalHours}h）`;
}

async function cmdPower() {
  const devId = argVal('--id', '');
  const wantJson = args.includes('--json');
  const watchRaw = argVal('--watch', '');
  const watchSec = args.includes('--watch') ? Math.max(1, Number(watchRaw || 15)) : 0;

  const store = createPowerStore({ log: (m) => console.log(m) });

  if (watchSec > 0) {
    const client = await connectMqtt();
    const filterTopic = devId ? [`nova/${devId}/up`] : ['nova/+/up'];
    client.subscribe(filterTopic, { qos: 1 });
    console.log(`监听电源事件 ${watchSec}s (${filterTopic[0]})...`);
    await new Promise((resolve) => {
      const t = setTimeout(resolve, watchSec * 1000);
      client.on('message', (topic, buf) => {
        const parts = topic.split('/');
        if (parts.length !== 3 || parts[0] !== 'nova' || parts[2] !== 'up') return;
        let data;
        try {
          data = JSON.parse(buf.toString('utf8'));
        } catch {
          return;
        }
        if (data?.evt === 'pwr') {
          store.recordPowerEvent(parts[1], data);
          if (!wantJson) {
            console.log(
              `[${parts[1]}] pwr why=${data.why} seq=${data.seq} ${data.pct}% (${data.mv}mV st=${data.st} vb=${data.vb ?? 0} vs=${data.vs ?? 0})`,
            );
          }
        } else if (data?.evt === 'stat') {
          store.recordStat(parts[1], data);
        }
      });
      process.once('SIGINT', () => {
        clearTimeout(t);
        resolve();
      });
    });
    await store.flush();
    client.end();
  }

  const report = store.getReport(devId || undefined);
  if (!report) {
    if (wantJson) {
      console.log(JSON.stringify({ deviceId: devId || null, storage: store.storageStats() }));
    } else {
      const st = store.storageStats();
      console.log(`暂无已记录的设备电源画像（数据目录: ${st.dataDir}）`);
    }
    return;
  }

  if (wantJson) {
    console.log(JSON.stringify(report, null, 2));
    return;
  }

  const evt = report.lastEvent;
  const st = report.lastStat;
  console.log(`设备 ${report.deviceId} 电源分析报告`);
  if (evt) {
    console.log(
      `最新关键帧 (${evt.why}): ${evt.pct}% | VBAT=${evt.mv}mV (comp=${evt.cmv}mV) | VBUS=${evt.vb}mV | VSYS=${evt.vs}mV | stage=${evt.stageName} | tmp=${evt.tmp}°C`,
    );
  }
  if (st) {
    console.log(
      `最新状态帧: ${st.pct}% (${st.mv}mV ${st.stageName}) | lcd=${st.lcd ? 'on' : 'off'} aod=${st.aod ? 'on' : 'off'} mhz=${st.mhz} rssi=${st.rssi}`,
    );
  }
  if (report.lastBoot) {
    const b = report.lastBoot;
    console.log(
      `最近开机黑匣子: on=[${b.onTags.join(',') || b.onSrc}] off=[${b.offTags.join(',') || b.offSrc}] bootMv=${b.bootMv} seed=${b.seed} lost=${b.lost}`,
    );
  }
  const cal = report.calibration;
  console.log(
    `校准参数: chgMohm=${cal.chgMohm ?? '-'} mΩ (累计变化 ${cal.mohmDelta >= 0 ? '+' : ''}${cal.mohmDelta} mΩ) | designMah=${cal.designMah ?? '-'} | probes=${cal.probeCount} | gauge偏差=${cal.avgGaugeBiasPct ?? '-'}%`,
  );
  console.log('续航消耗模型:');
  console.log(`  ${formatDrainRate('综合', report.drainRates.overall)}`);
  console.log(`  ${formatDrainRate('亮屏', report.drainRates.screenOn)}`);
  console.log(`  ${formatDrainRate('AOD', report.drainRates.aod)}`);
  console.log(`  ${formatDrainRate('息屏待机', report.drainRates.idle)}`);
  if (report.chargeProfile) {
    const cp = report.chargeProfile;
    console.log(
      `充电画像 (${cp.cycles} 周期 ${cp.totalHours}h): 均速=${cp.avgPctPerHour ?? '-'}%/h | VBUS最低=${cp.minVbusMv || '-'}mV | VSYS最低=${cp.minVsysMv || '-'}mV | 限流占比=${Math.round(cp.vbusLimRatio * 100)}% | 温控=${cp.thermalSec}s`,
    );
  }
  if (report.eta) {
    const eta = report.eta;
    const parts = [];
    if (eta.charging && eta.toFullMinutes != null) {
      parts.push(`预计充满还需 ${eta.toFullMinutes} 分钟 (${eta.toFullHours}h)`);
    }
    if (eta.remainHours != null) {
      parts.push(`当前电量(${eta.pct}%)按 ${eta.activeScenario} 预计可用 ${eta.remainHours}h`);
    }
    if (parts.length > 0) {
      console.log(`实时预估: ${parts.join(' | ')}`);
    }
  }
  if (report.alerts.length > 0) {
    console.log(`预警: ${report.alerts.join('；')}`);
  }
  const disk = report.storage;
  console.log(
    `磁盘占用: ${(disk.totalBytes / 1024).toFixed(1)} KiB（日志上限 ${(disk.maxLogTotalBytes / 1024).toFixed(0)} KiB，目录 ${disk.dataDir}）`,
  );
}

if (args[0] === 'auth' && args[1] === 'opendesign') {
  await cmdAuthOpenDesign();
} else if (args[0] === 'test-push') {
  await cmdTestPush();
} else if (args[0] === 'test-choreo') {
  await cmdTestChoreo();
} else if (args[0] === 'test-tts') {
  await cmdTestTts();
} else if (args[0] === 'power') {
  await cmdPower();
} else {
  console.log(
    '可用命令:\n' +
      '  nova-buddy auth opendesign --paste\n' +
      '  nova-buddy power [--id D93468] [--watch 15] [--json]\n' +
      '  nova-buddy test-push [--id D93468] [--approval]\n' +
      '  nova-buddy test-choreo [--id D93468]\n' +
      '  nova-buddy test-tts [--id D93468] [--text "..."] [--voice 冰糖]',
  );
}
