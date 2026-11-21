// Minimal WAV -> 16 kHz mono -> 4-bit IMA-ADPCM encoder ("NVAD" format).

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

export function parseWavPcm16(wavBuf) {
  if (!Buffer.isBuffer(wavBuf) || wavBuf.length < 44) {
    throw new Error('Invalid WAV buffer');
  }
  if (wavBuf.toString('ascii', 0, 4) !== 'RIFF' || wavBuf.toString('ascii', 8, 12) !== 'WAVE') {
    throw new Error('Not a RIFF/WAVE file');
  }

  let sampleRate = 24000;
  let channels = 1;
  let bitsPerSample = 16;
  let dataStart = -1;
  let dataSize = 0;

  let off = 12;
  while (off + 8 <= wavBuf.length) {
    const chunkId = wavBuf.toString('ascii', off, off + 4);
    const chunkSize = wavBuf.readUInt32LE(off + 4);
    const payloadStart = off + 8;
    if (chunkId === 'fmt ' && payloadStart + 16 <= wavBuf.length) {
      channels = wavBuf.readUInt16LE(payloadStart + 2);
      sampleRate = wavBuf.readUInt32LE(payloadStart + 4);
      bitsPerSample = wavBuf.readUInt16LE(payloadStart + 14);
    } else if (chunkId === 'data') {
      dataStart = payloadStart;
      dataSize = Math.min(chunkSize, wavBuf.length - payloadStart);
      break;
    }
    off = payloadStart + chunkSize + (chunkSize & 1);
  }

  if (dataStart < 0 || bitsPerSample !== 16 || channels < 1) {
    throw new Error(`Unsupported WAV format (ch=${channels}, bits=${bitsPerSample})`);
  }

  const frameBytes = channels * 2;
  const totalFrames = Math.floor(dataSize / frameBytes);
  const mono = new Int16Array(totalFrames);
  for (let i = 0; i < totalFrames; i++) {
    let sum = 0;
    for (let ch = 0; ch < channels; ch++) {
      sum += wavBuf.readInt16LE(dataStart + i * frameBytes + ch * 2);
    }
    mono[i] = Math.round(sum / channels);
  }
  return { sampleRate, samples: mono };
}

export function resampleLinear(samples, srcRate, dstRate = 16000) {
  if (srcRate === dstRate || samples.length === 0) return samples;
  const outLen = Math.max(1, Math.floor((samples.length * dstRate) / srcRate));
  const out = new Int16Array(outLen);
  const ratio = srcRate / dstRate;
  for (let i = 0; i < outLen; i++) {
    const srcPos = i * ratio;
    const idx = Math.floor(srcPos);
    const frac = srcPos - idx;
    const s0 = samples[idx];
    const s1 = idx + 1 < samples.length ? samples[idx + 1] : s0;
    out[i] = Math.round(s0 + (s1 - s0) * frac);
  }
  return out;
}

export function encodeNvAdpcm(samples, sampleRate = 16000) {
  const n = samples.length;
  const payloadBytes = Math.ceil(n / 2);
  const out = Buffer.allocUnsafe(16 + payloadBytes);

  out.write('NVAD', 0, 4, 'ascii');
  out.writeUInt32LE(sampleRate, 4);
  out.writeUInt32LE(n, 8);
  out.writeInt16LE(0, 12);
  out.writeUInt8(0, 14);
  out.writeUInt8(0, 15);

  let predictor = 0;
  let stepIndex = 0;
  for (let i = 0; i < n; i++) {
    const sample = samples[i];
    const step = STEP_TABLE[stepIndex];
    let diff = sample - predictor;
    let code = 0;
    if (diff < 0) {
      code = 8;
      diff = -diff;
    }
    if (diff >= step) {
      code |= 4;
      diff -= step;
    }
    if (diff >= (step >> 1)) {
      code |= 2;
      diff -= step >> 1;
    }
    if (diff >= (step >> 2)) {
      code |= 1;
    }

    let diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;
    if (code & 8) predictor -= diffq;
    else predictor += diffq;

    if (predictor > 32767) predictor = 32767;
    else if (predictor < -32768) predictor = -32768;

    stepIndex += INDEX_TABLE[code];
    if (stepIndex < 0) stepIndex = 0;
    else if (stepIndex > 88) stepIndex = 88;

    const byteIdx = 16 + (i >> 1);
    if ((i & 1) === 0) {
      out[byteIdx] = code & 0x0F;
    } else {
      out[byteIdx] |= (code & 0x0F) << 4;
    }
  }
  return out;
}

export function wavToNvAdpcm(wavBuf, targetRate = 16000) {
  const { sampleRate, samples } = parseWavPcm16(wavBuf);
  const resampled = resampleLinear(samples, sampleRate, targetRate);
  return encodeNvAdpcm(resampled, targetRate);
}
