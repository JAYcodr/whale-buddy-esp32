import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdir, mkdtemp, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { zstdCompressSync } from 'node:zlib';
import { createNovaBridge, shortenCommand, foldTitle, foldPath, sanitizeOneLine } from '../lib/bridge.js';
import { extractJsonObject, isQuietHours, normalizeChoreo, summarizeSessionReply, measureDisplayWidth } from '../lib/ai.js';
import { wavToNvAdpcm, encodeNvAdpcm, MAX_NVAD_SAMPLES } from '../lib/adpcm.js';

function createMockMqttClient() {
  const emitter = new EventEmitter();
  const published = [];
  return Object.assign(emitter, {
    connected: true,
    subscribed: [],
    published,
    subscribe(topics) {
      this.subscribed.push(topics);
    },
    publish(topic, payload, opts) {
      published.push({ topic, payload: JSON.parse(payload), opts });
    },
    end() {
      this.connected = false;
    },
  });
}

describe('Nova Bridge session aggregation & device prefs', () => {
  it('prioritizes waiting and running sessions over idle sessions and propagates turn errors', () => {
    const bridge = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883' });
    bridge.onSessionEvent({ id: 's1' }, { type: 'model/selection', data: { provider: 'deepseek', model: 'deepseek-v4' } });
    bridge.onSessionEvent({ id: 's1' }, { type: 'turn/start', data: { turn: 1 } });
    bridge.onSessionEvent(
      { id: 's1' },
      { type: 'tool/call', data: { turn: 1, step: 1, callId: 'c1', name: 'Bash', arguments: JSON.stringify({ command: 'pio run' }) } },
    );

    // An idle session updates later, should NOT shadow active s1 tool message
    bridge.onSessionEvent({ id: 's_idle' }, { type: 'model/selection', data: { provider: 'zhipu', model: 'glm-5' } });
    bridge.onSessionEvent({ id: 's_idle' }, { type: 'turn/end', data: { turn: 1, reason: { kind: 'completed' } } });
    let st = bridge.buildStatePayload();
    assert.equal(st.run, 1);
    assert.equal(st.sess, 2);
    assert.equal(st.msg, 'Bash: pio run');
    assert.equal(st.model, 'deepseek-v4');

    // Tool result updates message cleanly
    bridge.onSessionEvent({ id: 's1' }, { type: 'tool/result', data: { turn: 1, step: 1, message: { isError: false } } });
    st = bridge.buildStatePayload();
    assert.equal(st.msg, '整理 Bash 结果...');

    // Error turn end sets err=true, 且不再丢掉 reason.error 的原文
    bridge.onSessionEvent(
      { id: 's1' },
      {
        type: 'turn/end',
        data: { turn: 1, reason: { kind: 'error', error: { message: 'HTTP 429: rate limited', code: 'rate_limited' } } },
      },
    );
    st = bridge.buildStatePayload();
    assert.equal(st.run, 0);
    assert.equal(st.err, true);
    assert.equal(st.errMsg, 'HTTP 429: rate limited');
    assert.equal(st.msg, '本轮异常结束 · HTTP 429: rate limited');

    // 没带 message 的异常退化为固定文案，不能拼出空尾巴
    bridge.onSessionEvent({ id: 's1' }, { type: 'turn/end', data: { turn: 1, reason: { kind: 'error' } } });
    st = bridge.buildStatePayload();
    assert.equal(st.err, true);
    assert.equal(st.msg, '本轮异常结束');

    bridge.onSessionDisposed({ id: 's_idle' });
    bridge.stop();
  });

  it('respects device prefs (muted/tts disabled skips TTS, quiet=false overrides quietHours)', async () => {
    const mockClient = createMockMqttClient();
    const synthCalls = [];
    const bridge = createNovaBridge({
      mqttConnect: () => mockClient,
      fetchAllQuota: async () => ({ t: 'quota', cards: [] }),
      fetchModelDialRank: async () => null,
      synthesizeSpeech: async (text, opts) => {
        synthCalls.push({ text, opts });
        return Buffer.alloc(64, 1);
      },
    });
    bridge.start();

    // Device online with TTS muted (snd: 0) -> should skip TTS API entirely
    mockClient.emit(
      'message',
      'nova/D93468/hello',
      Buffer.from(JSON.stringify({ online: true, prefs: { snd: 0, tts: true, quiet: true, choreo: true } })),
    );
    assert.equal(await bridge.publishTts('静音测试', { force: true }), false);
    assert.equal(synthCalls.length, 0);

    // Device updates prefs: snd=75, tts=true, quiet=false -> TTS runs with ignoreQuiet=true
    mockClient.emit(
      'message',
      'nova/D93468/up',
      Buffer.from(JSON.stringify({ evt: 'prefs', snd: 75, tts: true, quiet: false, choreo: true })),
    );
    assert.equal(await bridge.publishTts('晚上好呀', { force: true }), true);
    assert.equal(synthCalls.length, 1);
    assert.equal(synthCalls[0].opts.ignoreQuiet, true);
    assert.ok(mockClient.published.some((p) => p.topic === 'nova/D93468/tts'));
    bridge.stop();
  });

  it('falls back immediately to next() when no device is online', async () => {
    const bridge = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883', fallbackMs: 100 });
    let nextCalled = false;
    const outcome = await bridge.onApprovalRequest(
      { toolName: 'Bash', reason: 'ls -la' },
      async () => {
        nextCalled = true;
        return 'allowed-once';
      },
    );
    assert.equal(nextCalled, true);
    assert.equal(outcome, 'allowed-once');
    bridge.stop();
  });
});

describe('AI choreography sanitizer and ADPCM encoder', () => {
  it('cleans think tags, fences, numeric speeds and second durations', () => {
    const raw =
      '<think>internal reasoning</think>\n```json\n' +
      '{"mood":"晚安摸鱼","steps":[{"x":999,"speed":1.4,"anim":"QUOTA","dur":3.5,"say":"看看余额"},{"x":20,"speed":0.7,"anim":"bogus","dur":500,"say":"休息一下"}]}\n```';
    const choreo = normalizeChoreo(extractJsonObject(raw));
    assert.equal(choreo.t, 'choreo');
    assert.equal(choreo.steps.length, 2);
    assert.deepEqual(choreo.steps[0], { x: 288, speed: 'run', anim: 'quota', dur: 10000, say: '看看余额' });
    assert.deepEqual(choreo.steps[1], { x: 80, speed: 'walk', anim: 'idle', dur: 10000, say: '休息一下' });
  });

  it('diversifies repeated animations across a 5-step choreography loop', () => {
    const rep = {
      mood: '反复发呆',
      steps: [
        { x: 100, speed: 'walk', anim: 'idle', dur: 11000, say: '1' },
        { x: 140, speed: 'walk', anim: 'idle', dur: 11000, say: '2' },
        { x: 180, speed: 'walk', anim: 'idle', dur: 11000, say: '3' },
        { x: 220, speed: 'walk', anim: 'idle', dur: 11000, say: '4' },
        { x: 260, speed: 'walk', anim: 'idle', dur: 11000, say: '5' },
      ],
    };
    const choreo = normalizeChoreo(rep, { night: false, recentAnims: ['idle'] });
    const anims = choreo.steps.map((s) => s.anim);
    assert.equal(new Set(anims).size, 5);
    assert.notEqual(anims[0], 'idle');
    assert.notEqual(anims[0], anims[4]);
  });

  it('evaluates overnight quiet hours accurately', () => {
    assert.equal(isQuietHours([23, 8], new Date('2026-11-21T23:30:00')), true);
    assert.equal(isQuietHours([23, 8], new Date('2026-11-21T07:15:00')), true);
    assert.equal(isQuietHours([23, 8], new Date('2026-11-21T14:00:00')), false);
  });

  it('resamples 24kHz WAV to 16kHz NVAD IMA-ADPCM container', () => {
    const samples24k = 2400; // 100 ms
    const wav = Buffer.alloc(44 + samples24k * 2);
    wav.write('RIFF', 0);
    wav.writeUInt32LE(36 + samples24k * 2, 4);
    wav.write('WAVE', 8);
    wav.write('fmt ', 12);
    wav.writeUInt32LE(16, 16);
    wav.writeUInt16LE(1, 20);
    wav.writeUInt16LE(1, 22);
    wav.writeUInt32LE(24000, 24);
    wav.writeUInt32LE(48000, 28);
    wav.writeUInt16LE(2, 32);
    wav.writeUInt16LE(16, 34);
    wav.write('data', 36);
    wav.writeUInt32LE(samples24k * 2, 40);
    for (let i = 0; i < samples24k; i++) {
      wav.writeInt16LE(Math.round(Math.sin(i * 0.1) * 12000), 44 + i * 2);
    }

    const adp = wavToNvAdpcm(wav, 16000);
    assert.equal(adp.toString('ascii', 0, 4), 'NVAD');
    assert.equal(adp.readUInt32LE(4), 16000);
    assert.equal(adp.readUInt32LE(8), 1600);
    assert.equal(adp.length, 16 + 800);
  });
});

describe('Nova Bridge dock 扩展字段', () => {
  it('把会话标题与上一个工具放进 state 载荷', () => {
    const bridge = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883' });
    bridge.onSessionEvent({ id: 's1' }, { type: 'session/title', data: { title: '  修渲染管线  ', messageSeqs: [], source: 'auto' } });
    bridge.onSessionEvent({ id: 's1' }, { type: 'turn/start', data: { turn: 1 } });
    bridge.onSessionEvent(
      { id: 's1' },
      { type: 'tool/call', data: { turn: 1, step: 1, callId: 'c1', name: 'bash', arguments: '{"command":"ls"}' } },
    );

    const st = bridge.buildStatePayload();
    assert.equal(st.title, '修渲染管线');
    assert.equal(st.tool, 'bash');
    // errMsg 只在异常时出现，正常回合不该带
    assert.equal(st.errMsg, undefined);
    bridge.stop();
  });

  it('标题超长时截断，且不把 err 改成对象', () => {
    const bridge = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883' });
    bridge.onSessionEvent({ id: 's1' }, { type: 'session/title', data: { title: '一二三四五六七八九十一二三四五六七八九十' } });
    bridge.onSessionEvent(
      { id: 's1' },
      { type: 'turn/end', data: { turn: 1, reason: { kind: 'error', error: { message: 'boom' } } } },
    );
    const st = bridge.buildStatePayload();
    assert.ok(st.title.length <= 20, `title 应截断到 20 字，实际 ${st.title.length}`);
    // 固件是 doc["err"] | false，只有字面量 true 能活
    assert.equal(typeof st.err, 'boolean');
    assert.equal(st.err, true);
    assert.equal(typeof st.errMsg, 'string');
    bridge.stop();
  });
});

describe('TTS 播报长度与去重', () => {
  it('长原因被压到设备能扛的长度', () => {
    const bridge = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883' });
    const longReason = 'A'.repeat(200);
    bridge.onSessionEvent(
      { id: 's1' },
      { type: 'turn/end', data: { turn: 1, reason: { kind: 'error', error: { message: longReason } } } },
    );
    // errMsg 自身限 120，state 载荷不该被 TTS 的 40 字上限影响
    const st = bridge.buildStatePayload();
    assert.equal(st.errMsg.length, 120);
    bridge.stop();
  });
});


describe('主屏文案清洗（v0.5.0）', () => {
  it('bash 命令去掉 cd/环境变量/内联脚本', () => {
    assert.equal(
      shortenCommand(`cd /tmp && node -e 'const mqtt = require("/x/y/z")'`),
      'node -e …',
    );
    assert.equal(shortenCommand('FOO=1 BAR=2 pio run -t upload'), 'pio run -t upload');
    assert.equal(shortenCommand('/usr/bin/bash -c "make -j8"'), 'make -j8');
    // 长命令截到 34 字（含省略号）
    const long = shortenCommand('./tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8');
    assert.equal(long.length, 34);
    assert.ok(long.endsWith('…'));
    // 多行命令压成一行
    assert.equal(shortenCommand('echo a\necho b'), 'echo a echo b');
    assert.equal(sanitizeOneLine('  a\tb  '), 'a b');
  });

  it('路径折叠到末两段，标题去掉路径形态', () => {
    assert.equal(foldPath('/Volumes/hamelin-ssd/x/src/ui/page_home.cpp'), 'ui/page_home.cpp');
    assert.equal(foldTitle('@/Volumes/hamelin-ssd/gemini工作区/nova-buddy'), 'nova-buddy');
    assert.equal(foldTitle('  issue 核查与复盘  '), 'issue 核查与复盘');
    const long = foldTitle('一二三四五六七八九十ABCDEFGHIJKLM');
    assert.ok(long.length <= 20);
    assert.ok(long.includes('…'));
  });

  it('bridge 的 state 载荷带清洗后的 msg/title', () => {
    const bridge = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883' });
    bridge.onSessionEvent({ id: 's1' }, { type: 'session/title', data: { title: '@/Volumes/hamelin-ssd/gemini工作区/nova-buddy' } });
    bridge.onSessionEvent(
      { id: 's1' },
      { type: 'tool/call', data: { name: 'bash', arguments: JSON.stringify({ command: `cd /tmp && node -e 'const mqtt = require("/x")'` }) } },
    );
    let st = bridge.buildStatePayload();
    assert.equal(st.title, 'nova-buddy');
    assert.equal(st.msg, 'bash: node -e …');
    assert.ok(!st.msg.includes('\n'));

    bridge.onSessionEvent(
      { id: 's1' },
      { type: 'tool/call', data: { name: 'read', arguments: JSON.stringify({ file_path: '/Volumes/x/src/ui/page_home.cpp' }) } },
    );
    st = bridge.buildStatePayload();
    assert.equal(st.msg, 'read: ui/page_home.cpp');
    bridge.stop();
  });
});

describe('剧本新鲜度（v0.5.0）', () => {
  const stub = async () => ({ t: 'choreo', mood: '测试', steps: [{ x: 184, speed: 'walk', anim: 'idle', dur: 12000, say: '晚安' }] });

  it('choreo 不 retain，并带 at 时间戳', async () => {
    const mockClient = createMockMqttClient();
    const bridge = createNovaBridge({ mqttConnect: () => mockClient, choreoGenerator: stub });
    bridge.start();
    mockClient.emit(
      'message',
      'nova/D93468/hello',
      Buffer.from(JSON.stringify({ online: true, prefs: { choreo: true } })),
    );
    const c = await bridge.refreshChoreo('test');
    assert.equal(c.t, 'choreo');
    const published = mockClient.published.filter((p) => p.topic === 'nova/D93468/choreo');
    assert.ok(published.length >= 1, '应该推给在线设备');
    const last = published[published.length - 1];
    assert.equal(last.opts.retain, false, 'retained 剧本会在冷启动时重放，必须关掉');
    assert.equal(typeof last.payload.at, 'number');
    assert.ok(Date.now() - last.payload.at < 5000);
    bridge.stop();
  });

  it('陈旧剧本在 hello 时不再补推', async () => {
    const mockClient = createMockMqttClient();
    const bridge = createNovaBridge({ mqttConnect: () => mockClient, choreoGenerator: stub, choreoFreshMs: 0 });
    bridge.start();
    mockClient.emit(
      'message',
      'nova/D93468/hello',
      Buffer.from(JSON.stringify({ online: true, prefs: { choreo: true } })),
    );
    await bridge.refreshChoreo('test');
    const before = mockClient.published.filter((p) => p.topic === 'nova/D93468/choreo').length;
    mockClient.emit(
      'message',
      'nova/D93468/hello',
      Buffer.from(JSON.stringify({ online: true, prefs: { choreo: true } })),
    );
    const after = mockClient.published.filter((p) => p.topic === 'nova/D93468/choreo').length;
    assert.equal(after, before, '超过新鲜期后 hello 不该再推旧剧本');
    bridge.stop();
  });

  it('persists bounded evt:pwr telemetry via MQTT up channel without logging stat heartbeats', async () => {
    const home = await mkdtemp(join(tmpdir(), 'nova-bridge-pwr-'));
    const mockClient = createMockMqttClient();
    const bridge = createNovaBridge({
      mqttConnect: () => mockClient,
      dshHome: home,
      fetchAllQuota: async () => ({ t: 'quota', cards: [] }),
      fetchModelDialRank: async () => null,
      tokensEnabled: false,
    });
    bridge.start();

    mockClient.emit(
      'message',
      'nova/D93468/up',
      Buffer.from(JSON.stringify({
        evt: 'pwr',
        seq: 1,
        why: 'boot',
        pct: 99,
        raw: 97,
        gauge: 100,
        st: 4,
        mv: 4211,
        cmv: 4159,
        rmv: 4162,
        vb: 5198,
        vs: 4426,
        ma: 300,
        ema: 50,
        tmp: 30,
        usb: 1,
        chg: 1,
        mohm: 620,
        mah: 600,
        onSrc: 4,
        offSrc: 8,
        seed: 100,
        lost: 0,
      })),
    );
    mockClient.emit(
      'message',
      'nova/D93468/up',
      Buffer.from(JSON.stringify({ evt: 'stat', mv: 4212, pct: 99, chg: 1, stage: 4, tmp: 30, lcd: 1, aod: 0 })),
    );

    await bridge.stop();
    const report = bridge.getPowerReport('D93468');
    assert.equal(report.lastBoot.onTags[0], 'vbus_insert');
    assert.equal(report.lastBoot.offTags[0], 'vsys_uvlo');
    assert.equal(report.lastStat.mv, 4212);
    const logStat = await stat(join(home, 'nova-buddy', 'power-events.jsonl'));
    const sumStat = await stat(join(home, 'nova-buddy', 'power-summary.json'));
    assert.ok(logStat.size > 0 && sumStat.size > 0);
  });
});

describe('命令方块桥接（sessionController + 压缩器 + NVAD clamp）', () => {
  it('clamps oversized NVAD TTS audio to <= 196500 samples (<= 98304 bytes) with smooth tail fade', () => {
    const longSamples = new Int16Array(220000);
    longSamples.fill(16000);
    const adp = encodeNvAdpcm(longSamples, 16000);
    assert.equal(adp.readUInt32LE(8), MAX_NVAD_SAMPLES);
    assert.ok(adp.length <= 98304, `NVAD bytes ${adp.length} exceeded 98304`);
  });

  it('compresses long assistant output cleanly and falls back without blind 40-char truncation', async () => {
    const short = await summarizeSessionReply({ assistantText: '单测全部通过了。' }, {});
    assert.equal(short, '单测全部通过了。');

    const longText = '好的，我已经帮你检查了全部代码结构，下面是详细分析：\n1. 修改了电池模型\n' + '细节内容'.repeat(30);
    const fallback = await summarizeSessionReply({ userText: '跑完了吗', assistantText: longText }, {});
    assert.ok(measureDisplayWidth(fallback) <= 40);
    assert.ok(!fallback.startsWith('好的，我已经'), 'must never blindly slice the opening preamble');
  });

  it('filters subagents, preserves lastAction across turn/end, and runs follow + prompt + rpcId claiming', async () => {
    const mockClient = createMockMqttClient();
    const promptCalls = [];
    const ttsCalls = [];

    const bridge = createNovaBridge({
      mqttConnect: () => mockClient,
      fetchAllQuota: async () => ({ t: 'quota', cards: [] }),
      fetchModelDialRank: async () => null,
      tokensEnabled: false,
      powerLogEnabled: false,
      replySummarizer: async () => '单测还剩 2 个失败。',
      synthesizeSpeech: async (text) => {
        ttsCalls.push(text);
        return Buffer.alloc(64, 1);
      },
    });
    bridge.start();

    mockClient.emit('message', 'nova/D93468/hello', Buffer.from(JSON.stringify({ online: true, prefs: { snd: 80, tts: true } })));

    // Main session + subagent session
    bridge.onSessionEvent({ id: 's1', header: { cwd: '/tmp/nova-buddy' } }, { type: 'session/title', data: { title: '电量估算四改' } });
    bridge.onSessionEvent({ id: 's1' }, { type: 'turn/start', data: {} });
    bridge.onSessionEvent({ id: 's1' }, { type: 'tool/call', data: { name: 'bash', arguments: JSON.stringify({ command: 'cargo test' }) } });
    bridge.onSessionEvent({ id: 's1' }, { type: 'turn/end', data: { reason: { kind: 'completed' } } });

    bridge.onSessionEvent({ id: 'sub_1', origin: 'subagent' }, { type: 'session/title', data: { title: '内部子代理' } });
    bridge.onSessionEvent({ id: 'sub_1', origin: 'subagent' }, { type: 'turn/start', data: {} });

    const sessPayload = bridge.buildSessionsPayload();
    assert.equal(sessPayload.items.length, 1, 'subagent must be filtered out');
    assert.equal(sessPayload.items[0].id, 's1');
    assert.equal(sessPayload.items[0].cwd, 'nova-buddy');
    assert.equal(sessPayload.items[0].act, 'bash: cargo test', 'turn/end must preserve lastAction');

    // Queue chat_send before sessionController arrives
    mockClient.emit(
      'message',
      'nova/D93468/up',
      Buffer.from(JSON.stringify({ evt: 'chat_send', id: 42, text: '把测试跑完', target: 's1', mode: 'queue' })),
    );
    await new Promise((r) => setTimeout(r, 10));

    // Attach mock sessionController
    const mockController = {
      async list() {
        return { items: [{ sessionId: 's1', title: '电量估算四改', cwd: '/tmp/nova-buddy', running: false }] };
      },
      async prompt(req) {
        promptCalls.push(req);
      },
      follow() {
        return (async function* () {
          const reqId = promptCalls[0]?.requestId;
          // Foreign turn/end BEFORE admitted must be ignored!
          yield { type: 'event', event: { type: 'turn/end', data: { reason: { kind: 'completed' } } } };
          yield { type: 'event', event: { type: 'user/message', data: { source: { kind: 'user', rpcId: reqId } } } };
          yield { type: 'event', event: { type: 'assistant/message', data: { message: { content: [{ type: 'text', text: '很长的回复'.repeat(20) }] } } } };
          yield { type: 'event', event: { type: 'turn/end', data: { reason: { kind: 'completed' } } } };
        })();
      },
    };
    bridge.setSessionController(mockController);
    await new Promise((r) => setTimeout(r, 30));

    assert.equal(promptCalls.length, 1);
    assert.equal(promptCalls[0].sessionId, 's1');
    assert.equal(promptCalls[0].mode, 'queue');

    const deliveries = mockClient.published
      .filter((p) => p.topic === 'nova/D93468/chat' && p.payload.t === 'delivery')
      .map((p) => p.payload);
    assert.ok(deliveries.some((d) => d.kind === 'run'));
    const reply = deliveries.find((d) => d.kind === 'reply');
    assert.ok(reply);
    assert.equal(reply.text, '单测还剩 2 个失败。');
    assert.ok(ttsCalls.includes('单测还剩 2 个失败。'));

    // Rejects waiting (approval) session
    bridge.onSessionEvent({ id: 's2' }, { type: 'session/title', data: { title: '等审批会话' } });
    bridge.onSessionEvent({ id: 's2' }, { type: 'approval/asked', data: { toolName: 'bash', reason: 'rm -rf' } });
    mockClient.emit(
      'message',
      'nova/D93468/up',
      Buffer.from(JSON.stringify({ evt: 'chat_send', id: 43, text: '继续', target: 's2', mode: 'steer' })),
    );
    await new Promise((r) => setTimeout(r, 20));
    const failWait = mockClient.published.find((p) => p.topic === 'nova/D93468/chat' && p.payload.kind === 'fail' && p.payload.target === 's2');
    assert.ok(failWait, 'must refuse injection into approval-waiting session');

    // Emits 'mute' when turn/end has no assistant text, and forwards mode='steer'
    const muteController = {
      async list() {
        return { items: [{ sessionId: 's1', title: '电量估算四改', cwd: '/tmp/nova-buddy', running: true }] };
      },
      async prompt(req) {
        promptCalls.push(req);
      },
      follow() {
        return (async function* () {
          const reqId = promptCalls[promptCalls.length - 1]?.requestId;
          yield { type: 'event', event: { type: 'user/message', data: { source: { kind: 'user', rpcId: reqId } } } };
          yield { type: 'event', event: { type: 'turn/end', data: { reason: { kind: 'completed' } } } };
        })();
      },
    };
    bridge.setSessionController(muteController);
    mockClient.emit(
      'message',
      'nova/D93468/up',
      Buffer.from(JSON.stringify({ evt: 'chat_send', id: 44, text: '停一下', target: 's1', mode: 'steer' })),
    );
    await new Promise((r) => setTimeout(r, 30));
    assert.equal(promptCalls[promptCalls.length - 1].mode, 'steer');
    const muteMsg = mockClient.published.find((p) => p.topic === 'nova/D93468/chat' && p.payload.id === 44 && p.payload.kind === 'mute');
    assert.ok(muteMsg, 'must publish explicit mute state when assistant produced no reply text');

    await bridge.stop();
  });
});

describe('导演上下文摘要（今日 Token）', () => {
  // DSH 会话日志是「逐条 append 的 zstd 多帧流」，造样本时必须真的压成多帧。
  const tokenLine = (time, usage) =>
    JSON.stringify({ type: 'assistant/message', time, data: { turn: 1, step: 1, usage } }) + '\n';
  const frame = (text) => zstdCompressSync(Buffer.from(text, 'utf8'));

  async function makeTokenHome(lines) {
    const home = await mkdtemp(join(tmpdir(), 'nova-bridge-tokens-'));
    const sessionDir = join(home, 'sessions', 'scope-a', 'session-1');
    await mkdir(sessionDir, { recursive: true });
    await writeFile(join(sessionDir, 'session.v4.jsonl.zstd'), Buffer.concat(lines.map(frame)));
    return home;
  }

  const captureBridge = (home, summaries) =>
    createNovaBridge({
      mqttConnect: () => createMockMqttClient(),
      dshHome: home,
      choreoGenerator: async (summary) => {
        summaries.push(summary);
        return { t: 'choreo', mood: '干饭', steps: [{ x: 184, speed: 'walk', anim: 'eat_token', dur: 12000, say: '干饭' }] };
      },
    });

  it('把最近一天的 Token 消耗写进上下文，导演才排得出 eat_token', async () => {
    const home = await makeTokenHome([
      tokenLine(new Date(2026, 10, 20, 12).getTime(), {
        inputTokens: 1_000, outputTokens: 10, cacheReadTokens: 9_000, cacheWriteTokens: 0,
      }),
      tokenLine(new Date(2026, 10, 21, 12).getTime(), {
        inputTokens: 2_000, outputTokens: 20, cacheReadTokens: 18_000, cacheWriteTokens: 0,
      }),
    ]);
    const summaries = [];
    const bridge = captureBridge(home, summaries);
    await bridge.refreshTokens(true);
    await bridge.refreshChoreo('test');
    assert.equal(summaries.length, 1);
    assert.match(summaries[0], /触发=test/);
    assert.match(summaries[0], /Token\(11\/21\)=2\.0万/, `上下文里没有今日 Token：${summaries[0]}`);
    bridge.stop();
  });

  it('没有 token 数据时宁可留空，也不硬塞一条 "Token=0"', async () => {
    const home = await mkdtemp(join(tmpdir(), 'nova-bridge-tokens-empty-'));
    const summaries = [];
    const bridge = captureBridge(home, summaries);
    await bridge.refreshTokens(true);
    await bridge.refreshChoreo('test');
    assert.equal(summaries.length, 1);
    assert.equal(summaries[0].includes('Token('), false, summaries[0]);
    bridge.stop();
  });
});
