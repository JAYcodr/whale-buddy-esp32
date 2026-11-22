/**
 * Token 用量聚合器 —— 为设备 `nova/<id>/tokens` 主题产出 payload。
 *
 * 数据源是 DSH 的会话日志（`$DSH_HOME/sessions/<scope>/<session>/session*.jsonl.zstd`），
 * 不经过任何服务：`ctx.sessions` 只列出当前进程内的 live session，拿不到历史，
 * 而累计页要的恰恰是历史。日志是逐条 append 的 zstd **多帧**流，Node 内置的
 * `createZstdDecompress` 解完第一帧就报 `Unknown frame descriptor`，所以这里按
 * magic 切帧后逐帧 `zstdDecompressSync`。
 *
 * 计数口径与 `@deepseek-ai/dsh-token-meter` 的 `tokenUsage` 投影保持一致：
 * 一条 `assistant/message`（或 `assistant/attempt`）贡献一次 usage，同一
 * `(turn, step)` 的后续样本**替换**前面的（重试/流式 settle），不是累加。
 *
 * 聚合结果按事件自身的 `time` 归日 —— 一个会话跨天时两天的数各自算对，
 * 不需要"按会话归日"这种近似。
 *
 * 缓存：`$DSH_HOME/nova-buddy/tokens-cache.json` 按文件 `mtimeMs + size` 增量，
 * 只看变过的文件，所以首次全量之后每次刷新只重解 1–2 个活跃会话。
 */

import { promises as fsp } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { zstdDecompressSync } from 'node:zlib';

const SESSION_FILE_SUFFIX = '.jsonl.zstd';
const ZSTD_MAGIC = Buffer.from([0x28, 0xb5, 0x2f, 0xfd]);
const CACHE_VERSION = 1;
const MAX_DAYS = 14;

const emptyBuckets = () => ({ uncached: 0, output: 0, cacheRead: 0, cacheWrite: 0 });

const bucketsTotal = (b) => b.uncached + b.output + b.cacheRead + b.cacheWrite;

/** 本地时区的 YYYY-MM-DD —— 设备上显示的也是本地日期。 */
function dayKey(ms) {
  const d = new Date(ms);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

/** 逐帧解压多帧 zstd；坏帧（通常是正在写入的最后一帧）直接跳过。 */
function forEachFrame(buf, onText) {
  const starts = [];
  let at = 0;
  for (;;) {
    const i = buf.indexOf(ZSTD_MAGIC, at);
    if (i === -1) break;
    starts.push(i);
    at = i + 4;
  }
  if (starts.length === 0) return 0;
  let frames = 0;
  for (let k = 0; k < starts.length; k += 1) {
    const end = k + 1 < starts.length ? starts[k + 1] : buf.length;
    try {
      onText(zstdDecompressSync(buf.subarray(starts[k], end)).toString('utf8'));
      frames += 1;
    } catch {
      /* 坏帧跳过：写入中的尾巴不该让整份文件作废 */
    }
  }
  return frames;
}

/** 一轮 assistant 的用量样本；取事件内嵌的 usage，退回流里的最后一个 usage chunk。 */
function usageOf(event) {
  const direct = event?.data?.usage;
  if (direct && typeof direct === 'object') return direct;
  const stream = event?.data?.stream;
  if (!Array.isArray(stream)) return null;
  for (let i = stream.length - 1; i >= 0; i -= 1) {
    const chunk = stream[i]?.chunk;
    if (chunk?.type === 'usage') return chunk.usage ?? chunk;
  }
  return null;
}

/** 解析一份日志，返回该文件的 { 'YYYY-MM-DD': buckets }。 */
function scanBuffer(buf) {
  const attempts = new Map(); // `${turn}:${step}` -> { usage, time }
  let tail = '';

  const handleLine = (line) => {
    if (!line) return;
    let event;
    try {
      event = JSON.parse(line);
    } catch {
      return;
    }
    if (event.type !== 'assistant/message' && event.type !== 'assistant/attempt') return;
    const usage = usageOf(event);
    if (!usage) return;
    const turn = event.data?.turn ?? 0;
    const step = event.data?.step ?? 0;
    attempts.set(`${turn}:${step}`, { usage, time: Number(event.time) || 0 });
  };

  forEachFrame(buf, (text) => {
    const lines = (tail + text).split('\n');
    tail = lines.pop() ?? '';
    for (const line of lines) handleLine(line);
  });
  if (tail) handleLine(tail);

  const days = {};
  for (const { usage, time } of attempts.values()) {
    const key = dayKey(time || Date.now());
    const bucket = days[key] ?? (days[key] = emptyBuckets());
    bucket.uncached += Number(usage.inputTokens) || 0;
    bucket.output += Number(usage.outputTokens) || 0;
    bucket.cacheRead += Number(usage.cacheReadTokens) || 0;
    bucket.cacheWrite += Number(usage.cacheWriteTokens) || 0;
  }
  return days;
}

/** 递归列出会话日志文件（只认 3 层深度内的 *.jsonl.zstd）。 */
async function listSessionFiles(root, depth = 0, out = []) {
  if (depth > 3) return out;
  let entries;
  try {
    entries = await fsp.readdir(root, { withFileTypes: true });
  } catch {
    return out;
  }
  for (const entry of entries) {
    if (entry.name.startsWith('.')) continue;
    const full = join(root, entry.name);
    if (entry.isDirectory()) {
      await listSessionFiles(full, depth + 1, out);
    } else if (entry.isFile() && entry.name.endsWith(SESSION_FILE_SUFFIX)) {
      out.push(full);
    }
  }
  return out;
}

/**
 * @param {object} [options]
 * @param {(msg: string) => void} [options.log]
 * @param {string} [options.dshHome] - 默认 `$DSH_HOME` 或 `~/.dsh`
 * @param {string} [options.sessionsDir] - 默认 `<dshHome>/sessions`
 * @param {string} [options.cachePath] - 默认 `<dshHome>/nova-buddy/tokens-cache.json`
 * @param {number} [options.maxDays] - days 最多几条，默认 14（与设备 MAX_TOKEN_DAYS 一致）
 * @param {boolean} [options.enabled] - 关掉就只返回上一次的 payload
 */
export function createTokenCollector(options = {}) {
  const log = options.log || (() => {});
  const dshHome = options.dshHome || process.env.DSH_HOME || join(homedir(), '.dsh');
  const sessionsDir = options.sessionsDir || join(dshHome, 'sessions');
  const cachePath = options.cachePath || join(dshHome, 'nova-buddy', 'tokens-cache.json');
  const maxDays = options.maxDays ?? MAX_DAYS;
  const enabled = options.enabled !== false;

  /** 缓存：path -> { mtimeMs, size, days } */
  const cache = { version: CACHE_VERSION, files: {} };
  let loaded = false;
  let running = null;
  let payload = null;
  let dirty = false;

  async function loadCache() {
    if (loaded) return;
    loaded = true;
    try {
      const raw = JSON.parse(await fsp.readFile(cachePath, 'utf8'));
      if (raw?.version === CACHE_VERSION && raw.files && typeof raw.files === 'object') {
        cache.files = raw.files;
      }
    } catch {
      /* 没有缓存（或缓存坏了）就从零开始 */
    }
  }

  async function saveCache() {
    if (!dirty) return;
    dirty = false;
    try {
      await fsp.mkdir(join(cachePath, '..'), { recursive: true });
      await fsp.writeFile(cachePath, JSON.stringify(cache));
    } catch (err) {
      log(`tokens: 缓存写入失败 ${err?.message || err}`);
    }
  }

  function buildPayload() {
    const byDay = new Map();
    const total = emptyBuckets();

    for (const entry of Object.values(cache.files)) {
      for (const [label, buckets] of Object.entries(entry.days || {})) {
        const acc = byDay.get(label) ?? emptyBuckets();
        for (const key of ['uncached', 'output', 'cacheRead', 'cacheWrite']) {
          acc[key] += buckets[key] || 0;
          total[key] += buckets[key] || 0;
        }
        byDay.set(label, acc);
      }
    }

    const labels = [...byDay.keys()].sort();
    const recent = labels.slice(-maxDays);

    // 命中率只看输入侧：输出不参与缓存，算进去会把数字抬高。
    const inputSide = total.cacheRead + total.uncached + total.cacheWrite;
    const cachePct = inputSide > 0 ? (total.cacheRead / inputSide) * 100 : 0;

    return {
      t: 'tokens',
      total: bucketsTotal(total),
      spanDays: labels.length,
      cachePct: Math.round(cachePct * 10) / 10,
      days: recent.map((label) => [label.slice(5).replace('-', '/'), bucketsTotal(byDay.get(label))]),
      // avg 故意不传：设备端按 days 自己算，单一实现，免得两边口径漂移
    };
  }

  async function refresh(force = false) {
    if (!enabled) return payload;
    if (running) return running;

    running = (async () => {
      await loadCache();

      const files = await listSessionFiles(sessionsDir);
      const seen = new Set();
      let scanned = 0;
      let touched = 0;

      for (const file of files) {
        seen.add(file);
        let stat;
        try {
          stat = await fsp.stat(file);
        } catch {
          continue;
        }
        const cached = cache.files[file];
        if (!force && cached && cached.mtimeMs === stat.mtimeMs && cached.size === stat.size) {
          continue;
        }
        try {
          const buf = await fsp.readFile(file);
          const days = scanBuffer(buf);
          cache.files[file] = { mtimeMs: stat.mtimeMs, size: stat.size, days };
          touched += 1;
          dirty = true;
        } catch (err) {
          log(`tokens: 跳过 ${file} (${err?.message || err})`);
        }
        scanned += 1;
        // 首次全量扫可能上千个文件；每 8 个让出一次事件循环，
        // 免得 MQTT 心跳和审批被扫描饿死。
        if (scanned % 8 === 0) await new Promise((r) => setImmediate(r));
      }

      for (const file of Object.keys(cache.files)) {
        if (!seen.has(file)) {
          delete cache.files[file];
          dirty = true;
        }
      }

      payload = buildPayload();
      if (touched > 0) {
        log(`tokens: ${touched} 个会话有更新 → ${payload.total} tokens / ${payload.spanDays} 天`);
        await saveCache();
      }
      return payload;
    })()
      .catch((err) => {
        log(`tokens: 聚合失败 ${err?.message || err}`);
        return payload;
      })
      .finally(() => {
        running = null;
      });

    return running;
  }

  return {
    refresh,
    peek: () => payload,
    cachePath,
  };
}
