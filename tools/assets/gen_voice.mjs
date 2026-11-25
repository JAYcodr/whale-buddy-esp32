#!/usr/bin/env node
// 用本机「听书值守」的本地 Qwen3-TTS 合成机内语音条 → 16kHz NVAD ADPCM。
//
// 用法：
//   node tools/assets/gen_voice.mjs                 # 生成全部（CLIPS 表）
//   node tools/assets/gen_voice.mjs click_wave_0    # 只生成指定 key
//   node tools/assets/gen_voice.mjs --list          # 列 key 与文本
//
// 走听书值守的 `/api/preview` + 未保存 draft，只对本次请求生效：强制 provider=local。
// 音色取听书值守已保存的 `upstream.local.refs.narrator`（wav + 转写文本），本脚本不改配置。
// 换参考音：在 DSH 里用 audiobook_tts_config 工具把 upstream.local.refs.narrator 的
// wav / text 换成目标参考音（text 必须是 wav 里那句话的转写），生成完再改回去 ——
// 别把读书用的旁白音色留在小孩声上。
// 端口与 token 从插件配置读，DSH 重启换端口也不受影响。
// 响度对齐现有语音条：RMS ≈ -20 dB，峰值用 alimiter（level=0）压在 -7 dB 以内，小喇叭防破。
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const { wavToNvAdpcm } = await import(path.join(ROOT, 'host-plugin/lib/adpcm.js'));

const OUT_DIR = path.join(ROOT, 'assets/audio');
const RAW_DIR = process.env.NOVA_TTS_RAW_DIR || '/tmp/nova-audio/raw';

const CLIPS = [
  ['click_wave_0',  '在呢主人！'],
  ['click_wave_1',  '随时待命哦～'],
  ['click_happy_0', '摸摸头充满电！'],
  ['click_happy_1', '今天也要加油！'],
  ['click_shy_0',   '突然戳我干嘛呀～'],
  ['click_shy_1',   '咦，别一直盯着看！'],
  ['click_giggle_0','嘿嘿，好痒呀～'],
  ['click_giggle_1','哈哈，别挠啦！'],
  ['click_angry_0', '别老戳人家啦！'],
  ['click_angry_1', '再戳要生气咯！'],
  ['wake_0',        '哇！没在偷懒！'],
  ['wake_1',        '唔……醒啦主人！'],
];

/** 听书值守落盘配置：独立服务端口 + 管理 token（未开启鉴权时不需要 token）。 */
function resolveTtsEndpoint() {
  if (process.env.NOVA_TTS_ENDPOINT) return process.env.NOVA_TTS_ENDPOINT;
  const home = process.env.HOME || '';
  const dirs = [
    process.env.AUDIOBOOK_TTS_DATA,
    process.env.MIMO_TTS_DATA,
    path.join(home, '.dsh/plugin-data/mimo-tts'),
    path.join(home, '.dsh/plugin-data/audiobook-tts-plugin'),
  ].filter(Boolean);
  for (const dir of dirs) {
    const file = path.join(dir, 'config.json');
    if (!fs.existsSync(file)) continue;
    const cfg = JSON.parse(fs.readFileSync(file, 'utf8'));
    const port = Number(cfg?.server?.port) || 8292;
    let url = `http://127.0.0.1:${port}/api/preview`;
    const token = String(cfg?.auth?.enabled ? cfg?.auth?.token || '' : '');
    if (token) url += `?token=${encodeURIComponent(token)}`;
    return url;
  }
  return 'http://127.0.0.1:8292/api/preview';
}

function ffmpegStats(wavPath) {
  const r = spawnSync('ffmpeg', ['-hide_banner', '-nostats', '-i', wavPath,
    '-af', 'astats=metadata=1:reset=0', '-f', 'null', '-'], { encoding: 'utf8' });
  return `${r.stdout || ''}\n${r.stderr || ''}`;
}

async function synth(text) {
  const body = JSON.stringify({
    text,
    role: 'narrator',
    // draft 只对本次请求生效：不写回配置，也不影响正在读的书。
    // 音色仍取已保存的 upstream.local.refs.narrator，脚本不碰配置。
    draft: { upstream: { provider: 'local' } },
  });
  const res = await fetch(resolveTtsEndpoint(), {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body,
    signal: AbortSignal.timeout(240000),
  });
  if (!res.ok) throw new Error(`preview ${res.status}: ${(await res.text()).slice(0, 200)}`);
  const ab = await res.arrayBuffer();
  if (ab.byteLength < 100) throw new Error('preview 返回空音频');
  return Buffer.from(ab);
}

function normalize(srcWav, dstWav) {
  const stats = ffmpegStats(srcWav).split('\n');
  const pick = (label) => {
    const line = stats.find((l) => l.includes(label));
    return line ? Number(line.split(':').pop()) : NaN;
  };
  const rms = pick('RMS level dB');
  const peak = pick('Peak level dB');
  const gain = Number.isFinite(rms) ? (-20 - rms) : 0;
  const limiterLin = Math.pow(10, -7 / 20);
  execFileSync('ffmpeg', ['-y', '-hide_banner', '-loglevel', 'error', '-i', srcWav,
    '-af', `volume=${gain.toFixed(2)}dB,alimiter=limit=${limiterLin.toFixed(4)}:attack=5:release=60:level=0`,
    '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', dstWav]);
  return { rms, peak, gain };
}

async function main() {
  const args = process.argv.slice(2);
  if (args.includes('--list')) {
    for (const [k, t] of CLIPS) console.log(k.padEnd(16), t);
    return;
  }
  const only = args.filter((a) => !a.startsWith('--'));
  const picks = only.length ? CLIPS.filter(([k]) => only.includes(k)) : CLIPS;
  if (!picks.length) throw new Error(`未知 key：${only.join(', ')}（--list 看清单）`);
  fs.mkdirSync(OUT_DIR, { recursive: true });
  fs.mkdirSync(RAW_DIR, { recursive: true });
  let totalBytes = 0;
  for (const [key, text] of picks) {
    const raw = path.join(RAW_DIR, `${key}.wav`);
    const norm = path.join(RAW_DIR, `${key}.16k.wav`);
    process.stdout.write(`${key} … ${text} `);
    fs.writeFileSync(raw, await synth(text));
    const m = normalize(raw, norm);
    const adp = wavToNvAdpcm(fs.readFileSync(norm), 16000);
    fs.writeFileSync(path.join(OUT_DIR, `${key}.adp`), adp);
    totalBytes += adp.length;
    const sec = ((adp.length - 16) * 2 / 16000).toFixed(2);
    console.log(`→ ${sec}s ${adp.length}B (源 rms ${m.rms.toFixed(1)}dB, peak ${m.peak.toFixed(1)}dB)`);
  }
  console.log(`done: ${picks.length} clips, ${(totalBytes / 1024).toFixed(1)} KB`);
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) main().catch((err) => { console.error('FAIL:', err.message); process.exit(1); });
