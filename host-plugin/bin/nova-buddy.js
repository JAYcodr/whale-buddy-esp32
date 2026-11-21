#!/usr/bin/env node
import readline from 'node:readline/promises';
import { stdin as input, stdout as output } from 'node:process';
import mqtt from 'mqtt';
import { fetchAllQuota, fetchModelDialRank, saveOpenDesignCookie } from '../lib/fetchers.js';

const args = process.argv.slice(2);
const brokerUrl = process.env.NOVA_MQTT_URL || 'mqtt://192.168.31.15:1883';

async function cmdAuthOpenDesign() {
  const idx = args.indexOf('--cookie');
  let raw = idx >= 0 ? args[idx + 1] : '';
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

async function cmdTestPush() {
  const idIdx = args.indexOf('--id');
  const devId = idIdx >= 0 ? args[idIdx + 1] : 'D93468';
  const withApproval = args.includes('--approval');

  console.log('正在抓取实时额度与 ModelDial 榜单...');
  const [quota, rank] = await Promise.all([fetchAllQuota(), fetchModelDialRank(true)]);
  console.log('Quota:', JSON.stringify(quota));
  console.log('Rank rows:', rank?.rows?.length ?? 0);

  const client = mqtt.connect(brokerUrl);
  await new Promise((resolve, reject) => {
    client.once('connect', resolve);
    client.once('error', reject);
  });

  const pub = (suffix, obj, retain = true) =>
    new Promise((res) =>
      client.publish(`nova/${devId}/${suffix}`, JSON.stringify(obj), { qos: 1, retain }, res),
    );

  const epoch = Math.floor(Date.now() / 1000);
  const tz = -new Date().getTimezoneOffset() * 60;
  await pub('time', { t: 'time', epoch, tz }, true);
  await pub(
    'state',
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
  await pub('quota', quota, true);
  if (rank) await pub('rank', rank, true);

  if (withApproval) {
    const apId = `ap_test_${Date.now().toString(36)}`;
    client.subscribe(`nova/${devId}/up`, { qos: 1 });
    await pub(
      'approval',
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
} else {
  console.log('可用命令:\n  nova-buddy auth opendesign --paste\n  nova-buddy test-push [--id D93468] [--approval]');
}
