#!/usr/bin/env node
// 云端 ASR 探针：拿本地录好的 WAV 分别打 DeepSeek 官方 ASR 与小米 MiMo ASR，对拍识别结果和耗时。
//
// 用法：
//   node tools/asr_probe.mjs                      # 扫 ./recordings 里所有 wav，两个 provider 都跑
//   node tools/asr_probe.mjs a.wav b.wav          # 指定文件
//   node tools/asr_probe.mjs --dir /path/to/wavs  # 换目录
//   node tools/asr_probe.mjs --provider mimo      # 只跑 deepseek | mimo | both（默认 both）
//   node tools/asr_probe.mjs --json out.json      # 结果同时落盘
//
// 凭据从 ~/.dsh/.credentials.yaml 的 refs 读（DEEPSEEK_API_KEY / XIAOMI_API_KEY），
// 同名环境变量优先；缺哪个就跳过哪条，不静默回退。
//
// 实测结论（2026-11-24）：MiMo 对低音量录音明显更扛得住；DeepSeek 官方那条有响度门限，
// 同一批 24dB 增益的录音里 4/6 直接返回空文本，+8dB 才勉强出半句。

import { existsSync, readFileSync, readdirSync, writeFileSync, mkdirSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { homedir } from 'node:os';

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(__dirname, '..');
const CREDENTIALS_PATH = process.env.DSH_CREDENTIALS ?? join(homedir(), '.dsh', '.credentials.yaml');
const RECORDINGS_DIR = join(REPO_ROOT, 'recordings');

/** Canonical DSH recordings are 16 kHz mono PCM16: 32000 bytes/s after the 44-byte header. */
const BYTES_PER_SECOND = 32000;
const WAV_HEADER_BYTES = 44;

function parseArgs(argv) {
  const opts = { files: [], dir: null, provider: 'both', json: null };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--dir') opts.dir = argv[++i];
    else if (arg === '--provider') opts.provider = argv[++i];
    else if (arg === '--json') opts.json = argv[++i];
    else if (arg === '--help' || arg === '-h') opts.help = true;
    else opts.files.push(arg);
  }
  return opts;
}

function usage() {
  return [
    '用法：node tools/asr_probe.mjs [文件...] [--dir <目录>] [--provider deepseek|mimo|both] [--json <out.json>]',
    `默认目录：${RECORDINGS_DIR}`,
    `凭据文件：${CREDENTIALS_PATH}（可用 DSH_CREDENTIALS 覆盖）`,
  ].join('\n');
}

/** Read one ref from the credentials YAML. Values may be quoted; trailing quotes/spaces are trimmed. */
function loadSecret(name) {
  const fromEnv = process.env[name];
  if (fromEnv) return fromEnv;
  if (!existsSync(CREDENTIALS_PATH)) return undefined;
  const text = readFileSync(CREDENTIALS_PATH, 'utf8');
  const match = text.match(new RegExp(`^\\s*${name}:\\s*["']?([^"'\\n]+?)["']?\\s*$`, 'm'));
  return match ? match[1].trim() : undefined;
}

function wavSeconds(wav) {
  return Math.max(0, (wav.length - WAV_HEADER_BYTES) / BYTES_PER_SECOND);
}

async function transcribeDeepSeek(wav, apiKey) {
  const form = new FormData();
  form.append('file', new Blob([new Uint8Array(wav)], { type: 'audio/wav' }), 'speech.wav');
  const startedAt = performance.now();
  const response = await fetch('https://api.deepseek.com/v1/tools/asr', {
    method: 'POST',
    headers: { Authorization: `Bearer ${apiKey}` },
    body: form,
  });
  const elapsedMs = performance.now() - startedAt;
  const raw = await response.text();
  let payload;
  try {
    payload = JSON.parse(raw);
  } catch {
    throw new Error(`HTTP ${response.status} 且不是 JSON：${raw.slice(0, 200)}`);
  }
  if (!response.ok) throw new Error(`HTTP ${response.status}: ${raw.slice(0, 300)}`);
  return { text: typeof payload.text === 'string' ? payload.text : '', elapsedMs };
}

async function transcribeMiMo(wav, apiKey, language = 'auto') {
  const encoded = Buffer.from(new Uint8Array(wav)).toString('base64');
  const payload = {
    model: 'mimo-v2.5-asr',
    messages: [{
      role: 'user',
      content: [{ type: 'input_audio', input_audio: { data: `data:audio/wav;base64,${encoded}` } }],
    }],
    asr_options: { language },
  };
  const startedAt = performance.now();
  const response = await fetch('https://api.xiaomimimo.com/v1/chat/completions', {
    method: 'POST',
    headers: { Authorization: `Bearer ${apiKey}`, 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  const elapsedMs = performance.now() - startedAt;
  const raw = await response.text();
  let decoded;
  try {
    decoded = JSON.parse(raw);
  } catch {
    throw new Error(`HTTP ${response.status} 且不是 JSON：${raw.slice(0, 200)}`);
  }
  if (!response.ok) throw new Error(`HTTP ${response.status}: ${raw.slice(0, 300)}`);
  const text = decoded?.choices?.[0]?.message?.content;
  return { text: typeof text === 'string' ? text : '', elapsedMs };
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help) {
    console.log(usage());
    return;
  }

  let files = opts.files.map((f) => resolve(f));
  if (files.length === 0) {
    const dir = opts.dir ? resolve(opts.dir) : RECORDINGS_DIR;
    if (!existsSync(dir)) {
      console.error(`目录不存在：${dir}\n${usage()}`);
      process.exitCode = 1;
      return;
    }
    files = readdirSync(dir)
      .filter((name) => name.toLowerCase().endsWith('.wav'))
      .sort()
      .map((name) => join(dir, name));
  }
  if (files.length === 0) {
    console.error('没有可处理的 wav 文件。');
    process.exitCode = 1;
    return;
  }

  const providers = [];
  if (opts.provider === 'both' || opts.provider === 'deepseek') {
    const apiKey = loadSecret('DEEPSEEK_API_KEY');
    providers.push({ id: 'deepseek', label: 'DeepSeek', apiKey, run: (wav) => transcribeDeepSeek(wav, apiKey) });
  }
  if (opts.provider === 'both' || opts.provider === 'mimo') {
    const apiKey = loadSecret('XIAOMI_API_KEY');
    providers.push({ id: 'mimo', label: 'MiMo', apiKey, run: (wav) => transcribeMiMo(wav, apiKey) });
  }
  for (const provider of providers) {
    if (!provider.apiKey) console.error(`⚠ ${provider.label}: 没找到 API key，跳过。`);
  }
  const active = providers.filter((provider) => provider.apiKey);

  const rows = [];
  for (const file of files) {
    let wav;
    try {
      wav = readFileSync(file);
    } catch (error) {
      console.error(`读不了 ${basename(file)}: ${error.message}`);
      continue;
    }
    const row = { file: basename(file), seconds: Number(wavSeconds(wav).toFixed(2)), results: {} };
    for (const provider of active) {
      try {
        const result = await provider.run(wav);
        row.results[provider.id] = { text: result.text, elapsedMs: Math.round(result.elapsedMs) };
        console.log(`${row.file}  ${row.seconds}s  ${provider.label.padEnd(8)} ${(result.elapsedMs / 1000).toFixed(2)}s  ${JSON.stringify(result.text)}`);
      } catch (error) {
        row.results[provider.id] = { error: error.message };
        console.log(`${row.file}  ${row.seconds}s  ${provider.label.padEnd(8)} ERROR  ${error.message}`);
      }
    }
    rows.push(row);
  }

  if (opts.json) {
    const outPath = resolve(opts.json);
    mkdirSync(dirname(outPath), { recursive: true });
    writeFileSync(outPath, `${JSON.stringify({ createdAt: new Date().toISOString(), rows }, null, 2)}\n`);
    console.log(`\n已写入 ${outPath}`);
  }

  const empty = rows.flatMap((row) =>
    Object.entries(row.results)
      .filter(([, r]) => r.error === undefined && r.text === '')
      .map(([id]) => `${row.file}/${id}`),
  );
  if (empty.length > 0) console.log(`\n空文本 ${empty.length} 条：${empty.join(', ')}`);
}

await main();
