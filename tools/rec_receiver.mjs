#!/usr/bin/env node
// Nova Buddy 本地录音接收器：订阅 MQTT 上的 16kHz PCM 分块流，保存为标准 WAV，
// 计算峰值 / RMS / 底噪 / 削波统计，并向设备回传 mic_ack 确认。
//
// 用法：
//   node tools/rec_receiver.mjs
//   node tools/rec_receiver.mjs --id D93468 --url mqtt://192.168.31.15:1883 --out ./recordings
//   node tools/rec_receiver.mjs --self-test

import { createRequire } from 'node:module';
import { existsSync, mkdirSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(__dirname, '..');

export function parseMicChunk(buf) {
  if (!Buffer.isBuffer(buf) || buf.length < 6) {
    throw new Error(`Invalid mic chunk buffer (len=${buf?.length ?? 0})`);
  }
  return {
    clipId: buf.readUInt16LE(0),
    seq: buf.readUInt16LE(2),
    total: buf.readUInt16LE(4),
    pcm: buf.subarray(6),
  };
}

export function buildWavBuffer(pcmBuf, sampleRate = 16000, channels = 1, bitsPerSample = 16) {
  const byteRate = sampleRate * channels * (bitsPerSample / 8);
  const blockAlign = channels * (bitsPerSample / 8);
  const dataSize = pcmBuf.length;
  const hdr = Buffer.allocUnsafe(44);

  hdr.write('RIFF', 0, 4, 'ascii');
  hdr.writeUInt32LE(36 + dataSize, 4);
  hdr.write('WAVE', 8, 4, 'ascii');
  hdr.write('fmt ', 12, 4, 'ascii');
  hdr.writeUInt32LE(16, 16);
  hdr.writeUInt16LE(1, 20); // PCM
  hdr.writeUInt16LE(channels, 22);
  hdr.writeUInt32LE(sampleRate, 24);
  hdr.writeUInt32LE(byteRate, 28);
  hdr.writeUInt16LE(blockAlign, 32);
  hdr.writeUInt16LE(bitsPerSample, 34);
  hdr.write('data', 36, 4, 'ascii');
  hdr.writeUInt32LE(dataSize, 40);

  return Buffer.concat([hdr, pcmBuf]);
}

export function analyzePcm16(pcmBuf, sampleRate = 16000) {
  const sampleCount = Math.floor(pcmBuf.length / 2);
  if (sampleCount === 0) {
    return {
      sampleCount: 0,
      durationSec: 0,
      peakDb: -96,
      rmsDb: -96,
      dcOffset: 0,
      clipCount: 0,
      clipPct: 0,
    };
  }

  let peakAbs = 0;
  let sum = 0;
  let sumSq = 0;
  let clipCount = 0;
  for (let i = 0; i < sampleCount; i++) {
    const s = pcmBuf.readInt16LE(i * 2);
    const a = s < 0 ? -s : s;
    if (a > peakAbs) peakAbs = a;
    if (a >= 32700) clipCount++;
    sum += s;
    sumSq += s * s;
  }

  const rms = Math.sqrt(sumSq / sampleCount);
  const peakDb = peakAbs > 0 ? 20 * Math.log10(peakAbs / 32768) : -96;
  const rmsDb = rms > 1 ? 20 * Math.log10(rms / 32768) : -96;
  return {
    sampleCount,
    durationSec: sampleCount / sampleRate,
    peakDb: Number(peakDb.toFixed(1)),
    rmsDb: Number(rmsDb.toFixed(1)),
    dcOffset: Math.round(sum / sampleCount),
    clipCount,
    clipPct: Number(((clipCount * 100) / sampleCount).toFixed(2)),
  };
}

export function nextClipIndex(outDir) {
  if (!existsSync(outDir)) return 1;
  let maxNum = 0;
  for (const name of readdirSync(outDir)) {
    const m = /^rec_(\d+)_/.exec(name);
    if (m) {
      const n = Number(m[1]);
      if (Number.isFinite(n) && n > maxNum) maxNum = n;
    }
  }
  return maxNum + 1;
}

function pad3(n) {
  return String(n).padStart(3, '0');
}

function formatTimeTag(date = new Date()) {
  const hh = String(date.getHours()).padStart(2, '0');
  const mm = String(date.getMinutes()).padStart(2, '0');
  const ss = String(date.getSeconds()).padStart(2, '0');
  return `${hh}${mm}${ss}`;
}

function runSelfTest() {
  const sampleRate = 16000;
  const totalSamples = 3200; // 0.2s test tone
  const pcm = Buffer.allocUnsafe(totalSamples * 2);
  for (let i = 0; i < totalSamples; i++) {
    const val = Math.round(Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 16384);
    pcm.writeInt16LE(val, i * 2);
  }

  // Split into 2048-byte chunks matching firmware format
  const kChunk = 2048;
  const totalChunks = Math.ceil(pcm.length / kChunk);
  const clipId = 7;
  const parts = [];
  for (let seq = 0; seq < totalChunks; seq++) {
    const off = seq * kChunk;
    const slice = pcm.subarray(off, Math.min(pcm.length, off + kChunk));
    const pkt = Buffer.allocUnsafe(6 + slice.length);
    pkt.writeUInt16LE(clipId, 0);
    pkt.writeUInt16LE(seq, 2);
    pkt.writeUInt16LE(totalChunks, 4);
    slice.copy(pkt, 6);
    const parsed = parseMicChunk(pkt);
    assert.equal(parsed.clipId, clipId);
    assert.equal(parsed.seq, seq);
    assert.equal(parsed.total, totalChunks);
    parts[parsed.seq] = parsed.pcm;
  }

  const reassembled = Buffer.concat(parts).subarray(0, pcm.length);
  assert.equal(Buffer.compare(reassembled, pcm), 0);

  const wav = buildWavBuffer(reassembled, 16000, 1, 16);
  assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
  assert.equal(wav.readUInt32LE(4), 36 + pcm.length);
  assert.equal(wav.toString('ascii', 8, 12), 'WAVE');
  assert.equal(wav.readUInt32LE(24), 16000);
  assert.equal(wav.readUInt16LE(34), 16);
  assert.equal(wav.readUInt32LE(40), pcm.length);

  const stats = analyzePcm16(reassembled, 16000);
  assert.equal(stats.sampleCount, totalSamples);
  assert.ok(stats.peakDb > -6.5 && stats.peakDb < -5.5);
  assert.ok(stats.rmsDb > -9.5 && stats.rmsDb < -8.5);
  assert.equal(stats.clipCount, 0);
  console.log('rec_receiver self-test OK:', stats);
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes('--self-test')) {
    runSelfTest();
    return;
  }

  const arg = (name, def = '') => {
    const i = args.indexOf(`--${name}`);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : def;
  };

  const brokerUrl = arg('url', process.env.NOVA_MQTT_URL || 'mqtt://192.168.31.15:1883');
  const targetId = arg('id', process.env.NOVA_DEVICE_ID || '+');
  const outDir = resolve(arg('out', join(REPO_ROOT, 'recordings')));

  mkdirSync(outDir, { recursive: true });
  let clipCounter = nextClipIndex(outDir);

  const require = createRequire(new URL('../host-plugin/', import.meta.url));
  const mqtt = require('mqtt');

  const subTopic = `nova/${targetId}/mic/+`;
  const client = mqtt.connect(brokerUrl, {
    clientId: `nova-rec-${process.pid}`,
    reconnectPeriod: 2000,
  });

  const sessions = new Map();

  function gcSessions() {
    const now = Date.now();
    for (const [k, sess] of sessions) {
      if (now - sess.updatedAt > 60000) sessions.delete(k);
    }
  }

  function ensureSession(devId, clipId) {
    const key = `${devId}:${clipId}`;
    let sess = sessions.get(key);
    if (!sess) {
      sess = {
        devId,
        clipId,
        sr: 16000,
        ch: 1,
        bits: 16,
        bytes: 0,
        totalChunks: 0,
        gainDb: 24,
        chunks: [],
        updatedAt: Date.now(),
      };
      sessions.set(key, sess);
    }
    sess.updatedAt = Date.now();
    return sess;
  }

  client.on('connect', () => {
    console.log(`[rec_receiver] 已连接 ${brokerUrl}`);
    console.log(`[rec_receiver] 订阅 ${subTopic} -> 输出目录 ${outDir}`);
    client.subscribe(subTopic, { qos: 1 }, (err) => {
      if (err) console.error('[rec_receiver] 订阅失败:', err.message);
    });
  });

  client.on('error', (err) => {
    console.error('[rec_receiver] MQTT 错误:', err.message);
  });

  client.on('message', (topic, payload) => {
    const m = /^nova\/([^/]+)\/mic\/(start|chunk|end)$/.exec(topic);
    if (!m) return;
    const devId = m[1];
    const kind = m[2];

    if (kind === 'start') {
      gcSessions();
      let meta;
      try {
        meta = JSON.parse(payload.toString('utf8'));
      } catch {
        return;
      }
      const clipId = Number(meta.id) || 0;
      const key = `${devId}:${clipId}`;
      const sess = {
        devId,
        clipId,
        sr: Number(meta.sr) || 16000,
        ch: Number(meta.ch) || 1,
        bits: Number(meta.bits) || 16,
        bytes: Number(meta.bytes) || 0,
        totalChunks: Number(meta.chunks) || 0,
        gainDb: Number(meta.gain) || 24,
        chunks: new Array(Number(meta.chunks) || 0),
        updatedAt: Date.now(),
      };
      sessions.set(key, sess);
      console.log(
        `[rec_receiver] <- ${devId} #${clipId} 开始接收 ` +
        `(${(sess.bytes / (sess.sr * 2)).toFixed(1)}s, ${sess.totalChunks} 块, 增益 ${sess.gainDb}dB)`
      );
      return;
    }

    if (kind === 'chunk') {
      let pkt;
      try {
        pkt = parseMicChunk(payload);
      } catch {
        return;
      }
      const sess = ensureSession(devId, pkt.clipId);
      if (pkt.total > 0 && sess.totalChunks === 0) {
        sess.totalChunks = pkt.total;
      }
      if (pkt.seq < (sess.totalChunks || 65535)) {
        sess.chunks[pkt.seq] = pkt.pcm;
      }
      return;
    }

    if (kind === 'end') {
      let meta;
      try {
        meta = JSON.parse(payload.toString('utf8'));
      } catch {
        return;
      }
      const clipId = Number(meta.id) || 0;
      const key = `${devId}:${clipId}`;
      const sess = sessions.get(key);
      if (!sess) return;
      sessions.delete(key);

      const expectedChunks = Number(meta.chunks) || sess.totalChunks || sess.chunks.length;
      const expectedBytes = Number(meta.bytes) || sess.bytes;
      let missing = 0;
      const ordered = [];
      for (let i = 0; i < expectedChunks; i++) {
        if (!sess.chunks[i]) {
          missing++;
        } else {
          ordered.push(sess.chunks[i]);
        }
      }

      const ackTopic = `nova/${devId}/mic_ack`;
      if (missing > 0 || ordered.length === 0) {
        const ack = JSON.stringify({ id: clipId, ok: false, missing });
        client.publish(ackTopic, ack, { qos: 1 });
        console.warn(`[rec_receiver] ! ${devId} #${clipId} 丢包 ${missing}/${expectedChunks} 块，已通知设备`);
        return;
      }

      let pcm = Buffer.concat(ordered);
      if (expectedBytes > 0 && pcm.length > expectedBytes) {
        pcm = pcm.subarray(0, expectedBytes);
      }

      const stats = analyzePcm16(pcm, sess.sr);
      const durTag = `${stats.durationSec.toFixed(1)}s`;
      const filename = `rec_${pad3(clipCounter++)}_${sess.gainDb}dB_${durTag}_${formatTimeTag()}.wav`;
      const outPath = join(outDir, filename);
      const wavBuf = buildWavBuffer(pcm, sess.sr, sess.ch, sess.bits);
      writeFileSync(outPath, wavBuf);

      const ack = JSON.stringify({ id: clipId, ok: true, file: filename, missing: 0 });
      client.publish(ackTopic, ack, { qos: 1 });

      console.log(
        `[rec_receiver] ✓ 已保存 ${outPath}\n` +
        `               时长=${stats.durationSec.toFixed(2)}s  峰值=${stats.peakDb}dBFS  ` +
        `均方=${stats.rmsDb}dBFS  直流偏移=${stats.dcOffset}  削波=${stats.clipCount}(${stats.clipPct}%)`
      );
    }
  });
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main();
}
