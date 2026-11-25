#!/usr/bin/env node
// NVAD（4-bit IMA-ADPCM, 16kHz）→ WAV，用于核对机内语音条的内容与响度。
//
// 用法：
//   node tools/assets/nvad_decode.mjs assets/audio/wake_0.adp /tmp/wake_0.wav
//   node tools/assets/nvad_decode.mjs assets/audio/*.adp --out /tmp/nova-voice   # 批量
//
// 编码侧在 host-plugin/lib/adpcm.js（wavToNvAdpcm）；本文件只做逆向，
// 用于 `ffmpeg astats` 量响度、送 ASR 对拍台词。
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const INDEX_TABLE = [
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8,
];
const STEP_TABLE = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
  253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
  3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
  12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];

const HEADER_BYTES = 16;

export function decodeNvAdpcm(adp) {
  if (adp.length < HEADER_BYTES || adp.toString('ascii', 0, 4) !== 'NVAD') {
    throw new Error('不是 NVAD 文件');
  }
  const sampleRate = adp.readUInt32LE(4);
  const sampleCount = Math.min(adp.readUInt32LE(8), (adp.length - HEADER_BYTES) * 2);
  const pcm = Buffer.alloc(44 + sampleCount * 2);
  pcm.write('RIFF', 0, 4, 'ascii');
  pcm.writeUInt32LE(36 + sampleCount * 2, 4);
  pcm.write('WAVE', 8, 4, 'ascii');
  pcm.write('fmt ', 12, 4, 'ascii');
  pcm.writeUInt32LE(16, 16);
  pcm.writeUInt16LE(1, 20);              // PCM
  pcm.writeUInt16LE(1, 22);              // mono
  pcm.writeUInt32LE(sampleRate, 24);
  pcm.writeUInt32LE(sampleRate * 2, 28);
  pcm.writeUInt16LE(2, 32);
  pcm.writeUInt16LE(16, 34);
  pcm.write('data', 36, 4, 'ascii');
  pcm.writeUInt32LE(sampleCount * 2, 40);

  let predictor = 0;
  let stepIndex = 0;
  for (let i = 0; i < sampleCount; i++) {
    const byte = adp[HEADER_BYTES + (i >> 1)];
    const code = (i & 1) ? (byte >> 4) & 0x0f : byte & 0x0f;
    const step = STEP_TABLE[stepIndex];
    let diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    predictor = (code & 8) ? predictor - diff : predictor + diff;
    if (predictor > 32767) predictor = 32767;
    else if (predictor < -32768) predictor = -32768;
    stepIndex += INDEX_TABLE[code];
    if (stepIndex < 0) stepIndex = 0;
    else if (stepIndex > 88) stepIndex = 88;
    pcm.writeInt16LE(predictor, 44 + i * 2);
  }
  return { sampleRate, sampleCount, wav: pcm };
}

function main() {
  const argv = process.argv.slice(2);
  const outIdx = argv.indexOf('--out');
  const outDir = outIdx >= 0 ? argv[outIdx + 1] : '';
  const rest = argv.filter((a, i) => !a.startsWith('--') && (outIdx < 0 || i !== outIdx + 1));
  // 支持 `in.adp out.wav` 的单文件写法：第二个参数不是 .adp 就当输出路径
  const single = !outDir && rest.length === 2 && !/\.adp$/i.test(rest[1]);
  const inputs = single ? [rest[0]] : rest;
  const explicitOut = single ? rest[1] : '';
  if (!inputs.length) {
    console.error('用法: node tools/assets/nvad_decode.mjs <in.adp> [out.wav] [--out dir]');
    process.exit(1);
  }
  for (const file of inputs) {
    const { sampleRate, sampleCount, wav } = decodeNvAdpcm(fs.readFileSync(file));
    const target = explicitOut || (outDir
      ? path.join(outDir, `${path.basename(file).replace(/\.adp$/i, '')}.wav`)
      : file.replace(/\.adp$/i, '.wav'));
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, wav);
    console.log(`${file} -> ${target} (${sampleRate}Hz, ${(sampleCount / sampleRate).toFixed(2)}s)`);
  }
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) main();
