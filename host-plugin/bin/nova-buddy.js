#!/usr/bin/env node
import readline from 'node:readline/promises';
import { stdin as input, stdout as output } from 'node:process';
import mqtt from 'mqtt';
import { fetchAllQuota, fetchModelDialRank, saveOpenDesignCookie } from '../lib/fetchers.js';
import { generateChoreography, loadAiConfig, synthesizeSpeechAdpcm } from '../lib/ai.js';

const args = process.argv.slice(2);
const brokerUrl = process.env.NOVA_MQTT_URL || 'mqtt://192.168.31.15:1883';

function argVal(flag, defVal = '') {
  const i = args.indexOf(flag);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : defVal;
}

async function connectMqtt() {
  const client = mqtt.connect(brokerUrl);
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

if (args[0] === 'auth' && args[1] === 'opendesign') {
  await cmdAuthOpenDesign();
} else if (args[0] === 'test-push') {
  await cmdTestPush();
} else if (args[0] === 'test-choreo') {
  await cmdTestChoreo();
} else if (args[0] === 'test-tts') {
  await cmdTestTts();
} else {
  console.log(
    '可用命令:\n' +
      '  nova-buddy auth opendesign --paste\n' +
      '  nova-buddy test-push [--id D93468] [--approval]\n' +
      '  nova-buddy test-choreo [--id D93468]\n' +
      '  nova-buddy test-tts [--id D93468] [--text "..."] [--voice 冰糖]',
  );
}
