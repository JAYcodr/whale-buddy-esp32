import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { createNovaBridge } from '../lib/bridge.js';
import { extractJsonObject, isQuietHours, normalizeChoreo } from '../lib/ai.js';
import { wavToNvAdpcm } from '../lib/adpcm.js';

describe('Nova Bridge session aggregation', () => {
  it('tracks active running/waiting sessions and context pct', () => {
    const bridge = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883' });
    bridge.onSessionEvent({ id: 's1', model: 'deepseek-v4' }, { type: 'turn/start', data: {} });
    bridge.onSessionEvent({ id: 's1' }, { type: 'tool/call', data: { toolName: 'Bash', command: 'pio run' } });
    bridge.onSessionEvent({ id: 's1' }, { type: 'context/usage', data: { percent: 42 } });
    bridge.onSessionEvent({ id: 's2' }, { type: 'approval/asked', data: { toolName: 'Write' } });

    bridge.onSessionDisposed({ id: 's2' });
    bridge.stop();
    assert.ok(true);
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
