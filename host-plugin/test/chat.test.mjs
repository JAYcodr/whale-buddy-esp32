import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  buildWavBuffer,
  createMicReassembler,
  loadMiMoApiKey,
  parseMicChunk,
  transcribeMiMoWav,
} from '../lib/asr.js';
import { generateChatReply } from '../lib/ai.js';
import { createNovaBridge } from '../lib/bridge.js';

function makeMicChunkPacket(clipId, seq, total, pcm) {
  const hdr = Buffer.alloc(6);
  hdr.writeUInt16LE(clipId, 0);
  hdr.writeUInt16LE(seq, 2);
  hdr.writeUInt16LE(total, 4);
  return Buffer.concat([hdr, pcm]);
}

function createMockMqttClient() {
  const emitter = new EventEmitter();
  const published = [];
  return Object.assign(emitter, {
    connected: true,
    subscribed: [],
    published,
    subscribe(topics, opts) {
      this.subscribed.push({ topics, opts });
    },
    publish(topic, payload, opts, cb) {
      published.push({ topic, payload: JSON.parse(payload), opts });
      cb?.(null);
    },
    end() {
      this.connected = false;
    },
  });
}

function flushMicrotasks() {
  return new Promise((resolve) => setTimeout(resolve, 15));
}

describe('MiMo ASR helpers & chunk reassembly', () => {
  it('parses 6-byte LE mic chunk header and builds valid 44-byte RIFF/WAVE buffer', () => {
    const pcm0 = Buffer.from([1, 2, 3, 4]);
    const pkt = makeMicChunkPacket(42, 1, 3, pcm0);
    const parsed = parseMicChunk(pkt);
    assert.equal(parsed.clipId, 42);
    assert.equal(parsed.seq, 1);
    assert.equal(parsed.total, 3);
    assert.deepEqual(parsed.pcm, pcm0);

    const wav = buildWavBuffer(pcm0, 16000, 1, 16);
    assert.equal(wav.length, 44 + 4);
    assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
    assert.equal(wav.readUInt32LE(4), 40);
    assert.equal(wav.toString('ascii', 8, 12), 'WAVE');
    assert.equal(wav.readUInt32LE(24), 16000);
    assert.equal(wav.readUInt16LE(34), 16);
    assert.equal(wav.toString('ascii', 36, 40), 'data');
    assert.equal(wav.readUInt32LE(40), 4);
    assert.deepEqual(wav.subarray(44), pcm0);
  });

  it('reassembles out-of-order chunks, reports missing chunks, and evicts stale sessions >60s', () => {
    let now = 1_000_000;
    const reassembler = createMicReassembler({ staleMs: 60_000, now: () => now });

    // Out-of-order delivery: chunk 1 arrives before start and chunk 0
    const p0 = Buffer.alloc(16, 0x11);
    const p1 = Buffer.alloc(16, 0x22);
    reassembler.chunk('DEV1', makeMicChunkPacket(7, 1, 2, p1));
    reassembler.start('DEV1', { id: 7, sr: 16000, ch: 1, bits: 16, bytes: 32, chunks: 2 });
    reassembler.chunk('DEV1', makeMicChunkPacket(7, 0, 2, p0));

    const okRes = reassembler.end('DEV1', { id: 7, chunks: 2, bytes: 32 });
    assert.equal(okRes.ok, true);
    assert.equal(okRes.id, 7);
    assert.equal(okRes.missing, 0);
    assert.equal(okRes.wav.length, 44 + 32);
    assert.deepEqual(okRes.wav.subarray(44, 60), p0);
    assert.deepEqual(okRes.wav.subarray(60, 76), p1);
    assert.equal(reassembler.size(), 0);

    // Missing chunk case
    reassembler.start('DEV1', { id: 8, sr: 16000, ch: 1, bits: 16, bytes: 48, chunks: 3 });
    reassembler.chunk('DEV1', makeMicChunkPacket(8, 0, 3, p0));
    reassembler.chunk('DEV1', makeMicChunkPacket(8, 2, 3, p1));
    const missRes = reassembler.end('DEV1', { id: 8, chunks: 3, bytes: 48 });
    assert.equal(missRes.ok, false);
    assert.equal(missRes.missing, 1);
    assert.equal(missRes.wav, null);

    // Stale session eviction (>60s)
    reassembler.start('DEV1', { id: 9, chunks: 2 });
    assert.equal(reassembler.size(), 1);
    now += 61_000;
    reassembler.evictStale();
    assert.equal(reassembler.size(), 0);
  });

  it('resolves XIAOMI_API_KEY in order: env -> DSH_CREDENTIALS -> aiCfg.tts.apiKey', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'nova-asr-cred-'));
    const credPath = join(dir, '.credentials.yaml');
    await writeFile(credPath, 'refs:\n  XIAOMI_API_KEY: "yaml-key-123"\n', 'utf8');

    const prevEnv = process.env.XIAOMI_API_KEY;
    const prevCred = process.env.DSH_CREDENTIALS;
    try {
      process.env.DSH_CREDENTIALS = credPath;
      process.env.XIAOMI_API_KEY = 'env-key-999';
      assert.equal(loadMiMoApiKey({ tts: { apiKey: 'cfg-key' } }), 'env-key-999');

      delete process.env.XIAOMI_API_KEY;
      assert.equal(loadMiMoApiKey({ tts: { apiKey: 'cfg-key' } }), 'yaml-key-123');

      process.env.DSH_CREDENTIALS = join(dir, 'missing.yaml');
      assert.equal(loadMiMoApiKey({ tts: { apiKey: 'cfg-key' } }), 'cfg-key');
    } finally {
      if (prevEnv === undefined) delete process.env.XIAOMI_API_KEY;
      else process.env.XIAOMI_API_KEY = prevEnv;
      if (prevCred === undefined) delete process.env.DSH_CREDENTIALS;
      else process.env.DSH_CREDENTIALS = prevCred;
    }
  });

  it('calls MiMo ASR endpoint and rejects empty/whitespace transcripts', async () => {
    const wav = buildWavBuffer(Buffer.alloc(320, 1));
    let seenPayload = null;
    const okRes = await transcribeMiMoWav(wav, 'test-key', {
      fetch: async (_url, init) => {
        seenPayload = JSON.parse(init.body);
        return {
          ok: true,
          status: 200,
          text: async () => JSON.stringify({ choices: [{ message: { content: '今天天气不错' } }] }),
        };
      },
    });
    assert.equal(okRes.ok, true);
    assert.equal(okRes.text, '今天天气不错');
    assert.equal(seenPayload.model, 'mimo-v2.5-asr');
    assert.equal(seenPayload.asr_options.language, 'auto');
    assert.ok(seenPayload.messages[0].content[0].input_audio.data.startsWith('data:audio/wav;base64,'));

    const emptyRes = await transcribeMiMoWav(wav, 'test-key', {
      fetch: async () => ({
        ok: true,
        status: 200,
        text: async () => JSON.stringify({ choices: [{ message: { content: '   \n ' } }] }),
      }),
    });
    assert.equal(emptyRes.ok, false);
    assert.equal(emptyRes.err, '未识别到语音');
  });
});

describe('AI chat reply generator', () => {
  it('returns concise fallback reply when no LLM provider is configured', async () => {
    const reply = await generateChatReply('查状态', [], '触发=chat，DSH运行=2，等待=1，DeepSeek余额18.5元', {});
    assert.equal(typeof reply, 'string');
    assert.ok(reply.length > 0 && reply.length <= 60);
    assert.ok(reply.includes('运行2任务'));
  });

  it('falls back from primary to secondary provider, strips markdown, and enforces <=60 chars', async () => {
    const origFetch = globalThis.fetch;
    const seenUrls = [];
    globalThis.fetch = async (url, init) => {
      seenUrls.push({ url, body: JSON.parse(init.body) });
      if (url.includes('primary')) {
        return { ok: false, status: 500, text: async () => 'upstream err' };
      }
      return {
        ok: true,
        status: 200,
        text: async () => JSON.stringify({}),
        json: async () => ({
          choices: [{
            message: {
              content: '<think>reasoning</think>**诺瓦**：当前一切正常！' + '加油'.repeat(40),
            },
          }],
        }),
      };
    };

    try {
      const reply = await generateChatReply(
        '现在怎么样',
        [{ role: 'user', content: 'hi' }, { role: 'assistant', content: '你好呀' }],
        '触发=chat，DSH运行=1，等待=0',
        {
          llm: {
            primary: { url: 'https://primary.test/v1/chat/completions', apiKey: 'k1', model: 'm1' },
            fallback: { url: 'https://fallback.test/v1/chat/completions', apiKey: 'k2', model: 'm2' },
          },
        },
      );
      assert.equal(seenUrls.length, 2);
      assert.equal(seenUrls[1].body.messages[0].role, 'system');
      assert.equal(seenUrls[1].body.messages[1].content, 'hi');
      assert.equal(seenUrls[1].body.messages[3].content, '现在怎么样');
      assert.ok(!reply.includes('**'));
      assert.ok(!reply.includes('<think>'));
      assert.ok(reply.length <= 60);
    } finally {
      globalThis.fetch = origFetch;
    }
  });
});

describe('Nova Bridge mic ASR + chat_send + chat_tts', () => {
  it('routes binary mic/chunk without JSON.parse warnings and publishes mic_ack + asr result', async () => {
    const mockClient = createMockMqttClient();
    const warns = [];
    const log = (msg) => {};
    log.warn = (msg) => warns.push(msg);

    let asrCalls = 0;
    const bridge = createNovaBridge({
      mqttConnect: () => mockClient,
      fetchAllQuota: async () => ({ t: 'quota', cards: [] }),
      fetchModelDialRank: async () => null,
      tokensEnabled: false,
      powerLogEnabled: false,
      log,
      transcribeAudio: async (wav) => {
        asrCalls++;
        assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
        return asrCalls === 1
          ? { ok: true, text: '帮我查一下状态', ms: 123 }
          : { ok: false, err: '未识别到语音', ms: 88 };
      },
    });
    bridge.start();
    mockClient.emit('connect');

    const subEntry = mockClient.subscribed.find((s) => Array.isArray(s.topics));
    assert.ok(subEntry.topics.includes('nova/+/mic/+'));

    // Clip 1: 2 binary chunks -> success
    const pcmA = Buffer.from([0x01, 0x80, 0xff, 0x7f, 0x00, 0x00]); // non-JSON binary bytes
    const pcmB = Buffer.from([0x10, 0x20, 0x30, 0x40]);
    mockClient.emit('message', 'nova/D93468/mic/start', Buffer.from(JSON.stringify({ id: 1, sr: 16000, ch: 1, bits: 16, chunks: 2 })));
    mockClient.emit('message', 'nova/D93468/mic/chunk', makeMicChunkPacket(1, 0, 2, pcmA));
    mockClient.emit('message', 'nova/D93468/mic/chunk', makeMicChunkPacket(1, 1, 2, pcmB));
    mockClient.emit('message', 'nova/D93468/mic/end', Buffer.from(JSON.stringify({ id: 1, chunks: 2 })));

    await flushMicrotasks();
    assert.equal(warns.length, 0, 'binary mic/chunk must not trigger JSON parse warnings');

    const ack1 = mockClient.published.find((p) => p.topic === 'nova/D93468/mic_ack' && p.payload.id === 1);
    assert.deepEqual(ack1.payload, { id: 1, ok: true, file: 'mimo_asr', missing: 0 });
    assert.equal(ack1.opts.qos, 1);

    const asr1 = mockClient.published.find((p) => p.topic === 'nova/D93468/chat' && p.payload.id === 1);
    assert.deepEqual(asr1.payload, { t: 'asr', id: 1, ok: true, text: '帮我查一下状态', ms: 123 });
    assert.equal(asr1.opts.retain, false);

    // Clip 2: empty transcript -> ok:false err:'未识别到语音'
    mockClient.emit('message', 'nova/D93468/mic/start', Buffer.from(JSON.stringify({ id: 2, chunks: 1 })));
    mockClient.emit('message', 'nova/D93468/mic/chunk', makeMicChunkPacket(2, 0, 1, pcmA));
    mockClient.emit('message', 'nova/D93468/mic/end', Buffer.from(JSON.stringify({ id: 2, chunks: 1 })));
    await flushMicrotasks();

    const asr2 = mockClient.published.find((p) => p.topic === 'nova/D93468/chat' && p.payload.id === 2);
    assert.deepEqual(asr2.payload, { t: 'asr', id: 2, ok: false, err: '未识别到语音' });

    // Clip 3: missing chunk -> mic_ack ok:false, missing:1, no chat publish
    mockClient.emit('message', 'nova/D93468/mic/start', Buffer.from(JSON.stringify({ id: 3, chunks: 2 })));
    mockClient.emit('message', 'nova/D93468/mic/chunk', makeMicChunkPacket(3, 0, 2, pcmA));
    mockClient.emit('message', 'nova/D93468/mic/end', Buffer.from(JSON.stringify({ id: 3, chunks: 2 })));
    await flushMicrotasks();

    const ack3 = mockClient.published.find((p) => p.topic === 'nova/D93468/mic_ack' && p.payload.id === 3);
    assert.deepEqual(ack3.payload, { id: 3, ok: false, missing: 1 });
    assert.equal(mockClient.published.some((p) => p.topic === 'nova/D93468/chat' && p.payload.id === 3), false);

    await bridge.stop();
  });

  it('handles chat_send with rolling 6-turn history, targeted TTS push, and chat_tts replay', async () => {
    const mockClient = createMockMqttClient();
    const seenHistories = [];
    const ttsCalls = [];

    const bridge = createNovaBridge({
      mqttConnect: () => mockClient,
      fetchAllQuota: async () => ({ t: 'quota', cards: [] }),
      fetchModelDialRank: async () => null,
      tokensEnabled: false,
      powerLogEnabled: false,
      chatReplyGenerator: async (text, history) => {
        seenHistories.push({ text, history });
        if (text === 'FAIL_ME') throw new Error('timeout');
        return `回复:${text}`;
      },
      synthesizeSpeech: async (text, opts) => {
        ttsCalls.push({ text, opts });
        return Buffer.alloc(64, 2);
      },
    });
    bridge.start();

    // Two online devices; replies should target ONLY the requesting device
    mockClient.emit('message', 'nova/DEV_A/hello', Buffer.from(JSON.stringify({ online: true, prefs: { snd: 80, tts: true } })));
    mockClient.emit('message', 'nova/DEV_B/hello', Buffer.from(JSON.stringify({ online: true, prefs: { snd: 80, tts: true } })));

    for (let i = 1; i <= 7; i++) {
      mockClient.emit('message', 'nova/DEV_A/up', Buffer.from(JSON.stringify({ evt: 'chat_send', id: i, text: `第${i}句` })));
      await flushMicrotasks();
    }

    // On 7th turn, prior history must be capped at 6 turns (12 messages: 6 user + 6 assistant)
    assert.equal(seenHistories.length, 7);
    assert.equal(seenHistories[0].history.length, 0);
    assert.equal(seenHistories[6].history.length, 12);
    assert.equal(seenHistories[6].history[0].content, '第1句');
    assert.equal(seenHistories[6].history[11].content, '回复:第6句');

    const devAReply = mockClient.published.find((p) => p.topic === 'nova/DEV_A/chat' && p.payload.id === 7);
    assert.deepEqual(devAReply.payload, { t: 'reply', id: 7, ok: true, text: '回复:第7句' });
    assert.equal(devAReply.opts.retain, false);

    // TTS was pushed only to DEV_A, never DEV_B
    assert.ok(mockClient.published.some((p) => p.topic === 'nova/DEV_A/tts'));
    assert.equal(mockClient.published.some((p) => p.topic === 'nova/DEV_B/tts'), false);

    // Error path emits ok:false, err:'回复超时'
    mockClient.emit('message', 'nova/DEV_A/up', Buffer.from(JSON.stringify({ evt: 'chat_send', id: 99, text: 'FAIL_ME' })));
    await flushMicrotasks();
    const errReply = mockClient.published.find((p) => p.topic === 'nova/DEV_A/chat' && p.payload.id === 99);
    assert.deepEqual(errReply.payload, { t: 'reply', id: 99, ok: false, err: '回复超时' });

    // chat_tts triggers targeted TTS replay
    const ttsCountBefore = ttsCalls.length;
    mockClient.emit('message', 'nova/DEV_B/up', Buffer.from(JSON.stringify({ evt: 'chat_tts', text: '再播一遍这个回复' })));
    await flushMicrotasks();
    assert.equal(ttsCalls.length, ttsCountBefore + 1);
    assert.equal(ttsCalls[ttsCalls.length - 1].text, '再播一遍这个回复');
    assert.ok(mockClient.published.some((p) => p.topic === 'nova/DEV_B/tts'));

    await bridge.stop();
  });
});
