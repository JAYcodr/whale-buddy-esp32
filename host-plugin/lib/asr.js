import { existsSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const DEFAULT_MIMO_ASR_URL = 'https://api.xiaomimimo.com/v1/chat/completions';
const DEFAULT_STALE_MS = 60 * 1000;
const DEFAULT_ASR_TIMEOUT_MS = 15 * 1000;

/**
 * 解析设备端上行的二进制语音分片包：
 * 6 字节小端包头 [clipId:u16, seq:u16, total:u16] + 原始 16kHz 16-bit 单声道 PCM 载荷。
 */
export function parseMicChunk(buf) {
  if (!buf) return null;
  const view = Buffer.isBuffer(buf)
    ? buf
    : ArrayBuffer.isView(buf)
      ? Buffer.from(buf.buffer, buf.byteOffset, buf.byteLength)
      : Buffer.from(buf);
  if (view.length < 6) return null;
  const clipId = view.readUInt16LE(0);
  const seq = view.readUInt16LE(2);
  const total = view.readUInt16LE(4);
  const pcm = Buffer.from(view.subarray(6));
  return { clipId, seq, total, pcm };
}

/**
 * 为裸 PCM 数据封装标准 44 字节 RIFF/WAVE 文件头（默认 16kHz / 1ch / 16bit）。
 */
export function buildWavBuffer(pcm, sampleRate = 16000, channels = 1, bitsPerSample = 16) {
  const pcmBuf = Buffer.isBuffer(pcm)
    ? pcm
    : ArrayBuffer.isView(pcm)
      ? Buffer.from(pcm.buffer, pcm.byteOffset, pcm.byteLength)
      : Buffer.from(pcm || []);
  const dataSize = pcmBuf.length;
  const blockAlign = Math.max(1, Math.floor(channels * (bitsPerSample / 8)));
  const byteRate = sampleRate * blockAlign;

  const header = Buffer.alloc(44);
  header.write('RIFF', 0, 'ascii');
  header.writeUInt32LE(36 + dataSize, 4);
  header.write('WAVE', 8, 'ascii');
  header.write('fmt ', 12, 'ascii');
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(byteRate, 28);
  header.writeUInt16LE(blockAlign, 32);
  header.writeUInt16LE(bitsPerSample, 34);
  header.write('data', 36, 'ascii');
  header.writeUInt32LE(dataSize, 40);

  return Buffer.concat([header, pcmBuf]);
}

function parseJsonPayload(raw) {
  if (raw && typeof raw === 'object' && !Buffer.isBuffer(raw) && !ArrayBuffer.isView(raw)) {
    return raw;
  }
  const text = Buffer.isBuffer(raw) ? raw.toString('utf8') : String(raw || '');
  return JSON.parse(text);
}

/**
 * 按 deviceId:clipId 维护录音分片重组会话，自动淘汰超过 60 秒的过期会话。
 */
export function createMicReassembler(opts = {}) {
  const staleMs = Number(opts.staleMs ?? DEFAULT_STALE_MS);
  const nowFn = typeof opts.now === 'function' ? opts.now : Date.now;
  const sessions = new Map();

  function evictStale() {
    const now = nowFn();
    for (const [key, sess] of sessions.entries()) {
      if (now - sess.updatedAt > staleMs) {
        sessions.delete(key);
      }
    }
  }

  function start(deviceId, rawMeta) {
    evictStale();
    let meta;
    try {
      meta = parseJsonPayload(rawMeta);
    } catch {
      return null;
    }
    const clipId = Number(meta?.id);
    if (!Number.isInteger(clipId) || clipId < 0) return null;

    const key = `${deviceId}:${clipId}`;
    const prev = sessions.get(key);
    // 若 QoS 0 chunk 比 QoS 1 start 早到，保留已收到的分片只补全元信息；
    // 若同一 clipId 之前已有 start，则开启全新会话。
    const chunks = prev && !prev.hasStart ? prev.chunks : new Map();
    const session = {
      deviceId,
      clipId,
      hasStart: true,
      sr: Number(meta.sr) || 16000,
      ch: Number(meta.ch) || 1,
      bits: Number(meta.bits) || 16,
      expectedBytes: Number(meta.bytes) || 0,
      expectedChunks: Number(meta.chunks) || prev?.totalFromChunk || 0,
      meta,
      chunks,
      totalFromChunk: prev?.totalFromChunk || 0,
      updatedAt: nowFn(),
    };
    sessions.set(key, session);
    return { id: clipId, meta };
  }

  function chunk(deviceId, buf) {
    evictStale();
    const parsed = parseMicChunk(buf);
    if (!parsed || parsed.total === 0 || parsed.seq >= parsed.total) return null;

    const key = `${deviceId}:${parsed.clipId}`;
    let session = sessions.get(key);
    if (!session) {
      session = {
        deviceId,
        clipId: parsed.clipId,
        hasStart: false,
        sr: 16000,
        ch: 1,
        bits: 16,
        expectedBytes: 0,
        expectedChunks: parsed.total,
        meta: null,
        chunks: new Map(),
        totalFromChunk: parsed.total,
        updatedAt: nowFn(),
      };
      sessions.set(key, session);
    } else {
      session.totalFromChunk = Math.max(session.totalFromChunk, parsed.total);
      if (!session.expectedChunks) session.expectedChunks = parsed.total;
      session.updatedAt = nowFn();
    }

    session.chunks.set(parsed.seq, parsed.pcm);
    return {
      id: parsed.clipId,
      seq: parsed.seq,
      total: parsed.total,
      received: session.chunks.size,
    };
  }

  function end(deviceId, rawEnd) {
    evictStale();
    let endMeta;
    try {
      endMeta = parseJsonPayload(rawEnd);
    } catch {
      return null;
    }
    const clipId = Number(endMeta?.id);
    if (!Number.isInteger(clipId) || clipId < 0) return null;

    const key = `${deviceId}:${clipId}`;
    const session = sessions.get(key);
    sessions.delete(key);

    const expectedFromEnd = Number(endMeta?.chunks) || 0;
    if (!session) {
      return {
        id: clipId,
        ok: false,
        wav: null,
        missing: expectedFromEnd > 0 ? expectedFromEnd : 1,
        meta: endMeta,
      };
    }

    const totalChunks = expectedFromEnd || session.expectedChunks || session.totalFromChunk || 0;
    if (totalChunks <= 0) {
      return {
        id: clipId,
        ok: false,
        wav: null,
        missing: 1,
        meta: session.meta || endMeta,
      };
    }

    let missing = 0;
    const ordered = [];
    for (let seq = 0; seq < totalChunks; seq++) {
      const part = session.chunks.get(seq);
      if (!part) {
        missing++;
      } else {
        ordered.push(part);
      }
    }

    if (missing > 0) {
      return {
        id: clipId,
        ok: false,
        wav: null,
        missing,
        meta: session.meta || endMeta,
      };
    }

    const pcm = Buffer.concat(ordered);
    if (pcm.length === 0) {
      return {
        id: clipId,
        ok: false,
        wav: null,
        missing: totalChunks,
        meta: session.meta || endMeta,
      };
    }

    const endBytes = Number(endMeta?.bytes) > 0 ? Number(endMeta.bytes) : 0;
    const expectedBytes = endBytes || session.expectedBytes || 0;
    if (
      (session.expectedBytes > 0 && endBytes > 0 && session.expectedBytes !== endBytes) ||
      (expectedBytes > 0 && pcm.length !== expectedBytes)
    ) {
      return {
        id: clipId,
        ok: false,
        wav: null,
        missing: 0,
        err: '音频不完整',
        meta: session.meta || endMeta,
      };
    }

    const wav = buildWavBuffer(pcm, session.sr, session.ch, session.bits);
    return {
      id: clipId,
      ok: true,
      wav,
      missing: 0,
      meta: session.meta || endMeta,
    };
  }

  return {
    start,
    chunk,
    end,
    evictStale,
    size: () => sessions.size,
  };
}

/**
 * 按优先级解析 XIAOMI_API_KEY：
 * 1. process.env.XIAOMI_API_KEY
 * 2. ~/.dsh/.credentials.yaml（可用 DSH_CREDENTIALS 覆盖路径）
 * 3. aiCfg?.tts?.apiKey
 */
export function loadMiMoApiKey(aiCfg) {
  const fromEnv = process.env.XIAOMI_API_KEY;
  if (fromEnv && String(fromEnv).trim()) return String(fromEnv).trim();

  const credPath = process.env.DSH_CREDENTIALS ?? join(homedir(), '.dsh', '.credentials.yaml');
  try {
    if (credPath && existsSync(credPath)) {
      const text = readFileSync(credPath, 'utf8');
      const match = text.match(/^\s*XIAOMI_API_KEY:\s*["']?([^"'\n]+?)["']?\s*$/m);
      if (match && match[1].trim()) return match[1].trim();
    }
  } catch {
    // 忽略凭据文件读取异常，继续回退到 aiCfg
  }

  const fromCfg = aiCfg?.tts?.apiKey;
  return fromCfg && String(fromCfg).trim() ? String(fromCfg).trim() : '';
}

function extractChoiceText(decoded) {
  const content = decoded?.choices?.[0]?.message?.content;
  if (typeof content === 'string') return content.trim();
  if (Array.isArray(content)) {
    return content
      .map((part) => (typeof part === 'string' ? part : typeof part?.text === 'string' ? part.text : ''))
      .join(' ')
      .trim();
  }
  return '';
}

/**
 * 调用小米 MiMo ASR（mimo-v2.5-asr）识别 WAV 音频，15s 超时；
 * 空文本一律视为识别失败（{ ok: false, err: '未识别到语音' }）。
 */
export async function transcribeMiMoWav(wav, apiKey, opts = {}) {
  const startedAt = Date.now();
  if (!apiKey || !String(apiKey).trim()) {
    return { ok: false, err: '未配置ASR密钥', ms: 0 };
  }
  const wavBuf = Buffer.isBuffer(wav) ? wav : Buffer.from(wav || []);
  if (wavBuf.length <= 44) {
    return { ok: false, err: '音频为空', ms: 0 };
  }

  const fetchFn = opts.fetch || globalThis.fetch;
  const url = opts.url || DEFAULT_MIMO_ASR_URL;
  const timeoutMs = Number(opts.timeoutMs ?? DEFAULT_ASR_TIMEOUT_MS);
  const encoded = wavBuf.toString('base64');

  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetchFn(url, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${String(apiKey).trim()}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({
        model: 'mimo-v2.5-asr',
        messages: [
          {
            role: 'user',
            content: [
              {
                type: 'input_audio',
                input_audio: { data: `data:audio/wav;base64,${encoded}` },
              },
            ],
          },
        ],
        asr_options: { language: opts?.language || 'auto' },
      }),
      signal: ctrl.signal,
    });
    const ms = Math.max(0, Date.now() - startedAt);
    const raw = await res.text();
    let decoded;
    try {
      decoded = JSON.parse(raw);
    } catch {
      return { ok: false, err: `ASR HTTP ${res.status}`, ms };
    }
    if (!res.ok) {
      return { ok: false, err: `ASR HTTP ${res.status}`, ms };
    }
    const text = extractChoiceText(decoded);
    if (!text) {
      return { ok: false, err: '未识别到语音', ms };
    }
    return { ok: true, text, ms };
  } catch (err) {
    const ms = Math.max(0, Date.now() - startedAt);
    if (err?.name === 'AbortError') {
      return { ok: false, err: '识别超时', ms };
    }
    const detail = String(err?.message || '')
      .replace(/[\u0000-\u001f\u007f]+/g, ' ')
      .replace(/\s+/g, ' ')
      .trim()
      .slice(0, 60);
    return { ok: false, err: detail || '识别失败', ms };
  } finally {
    clearTimeout(timer);
  }
}
