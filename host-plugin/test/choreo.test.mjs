// v0.6.0 24 动作扩容：normalizeChoreo 白名单 / 回退 / 防重复，以及三端动作表的同步性。
// 契约：docs/pet-expressiveness-plan.md §1（索引 0..23）。
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { normalizeChoreo } from '../lib/ai.js';

// 顺序必须与 src/anim/nva.h 的 NvaAnimKey、src/net/proto.cpp 的 kAnimNames 逐项一致。
const ALL_ANIMS = [
  'idle', 'thinking', 'working', 'waiting', 'success', 'error', 'click', 'sleep', 'quota',
  'click_happy', 'click_shy', 'click_angry', 'click_giggle',
  'crab_walk', 'happy_run', 'coding', 'archive',
  'stretch', 'yawn', 'wake_up', 'hum', 'eat_token', 'bubble', 'quota_worry',
];
// ai.js 夜间防重复池（这里按契约重抄一份：池子改了这条测试会红，逼人回来对齐）。
const NIGHT_POOL = ['sleep', 'idle', 'waiting', 'thinking', 'quota', 'yawn', 'stretch', 'hum', 'bubble', 'wake_up'];

const step = (anim, extra = {}) => ({ x: 184, speed: 'walk', anim, dur: 12000, say: '', ...extra });

const assertNoRepeatInLoop = (anims) => {
  assert.ok(anims.length > 1);
  anims.forEach((anim, i) => {
    assert.ok(ALL_ANIMS.includes(anim), `${anim} 不在 24 动作白名单`);
    assert.notEqual(anim, anims[(i + 1) % anims.length], `第 ${i} 步与下一步撞了同一个动作 ${anim}`);
  });
};

describe('choreo 24 动作：白名单、回退与防重复', () => {
  it('24 个动作名逐个原样保留（合法动作不许被改写）', () => {
    assert.equal(ALL_ANIMS.length, 24);
    assert.equal(new Set(ALL_ANIMS).size, 24, '动作表里有重复项');
    for (const anim of ALL_ANIMS) {
      const choreo = normalizeChoreo({ mood: '逐个验', steps: [step(anim)] });
      assert.equal(choreo.steps[0].anim, anim, `${anim} 被改写成了 ${choreo.steps[0].anim}`);
    }
  });

  it('未知名 / 空串 / 缺字段一律回落 idle', () => {
    for (const bad of ['quota_empty', 'eat_tokens', 'idle_v2', 'wakeup', '', null, 42]) {
      const choreo = normalizeChoreo({ mood: '回退', steps: [{ ...step('idle'), anim: bad }] });
      assert.equal(choreo.steps[0].anim, 'idle', `anim=${JSON.stringify(bad)} 没回落 idle`);
    }
    const missing = normalizeChoreo({ steps: [{ x: 150, dur: 12000 }] });
    assert.equal(missing.steps[0].anim, 'idle');
  });

  it('大小写与首尾空白按名字归一（LLM 常把动作写成大写）', () => {
    for (const [raw, want] of [['CLICK_HAPPY', 'click_happy'], ['  Bubble ', 'bubble'], ['Quota_Worry', 'quota_worry']]) {
      const choreo = normalizeChoreo({ mood: '归一', steps: [{ ...step('idle'), anim: raw }] });
      assert.equal(choreo.steps[0].anim, want);
    }
  });

  it('六步全是合法且互不相同的动作时原封不动', () => {
    const anims = ['coding', 'archive', 'stretch', 'yawn', 'hum', 'bubble'];
    const choreo = normalizeChoreo({
      mood: '写代码摸鱼',
      steps: anims.map((anim, i) => step(anim, { x: 90 + i * 30 })),
    });
    assert.deepEqual(choreo.steps.map((s) => s.anim), anims);
  });

  it('相邻重复与首尾环绕都换成池子里没用过的动作', () => {
    const anims = ['stretch', 'stretch', 'yawn', 'yawn', 'hum', 'hum'];
    const out = normalizeChoreo({ mood: '伸懒腰', steps: anims.map((anim) => step(anim)) })
      .steps.map((s) => s.anim);
    assertNoRepeatInLoop(out);
    // 被换掉的必须真的是新动作，而不是把重复项原样留着
    assert.equal(out.length, anims.length);
  });

  it('末步与首步相同会被改写（剧本是循环播放的）', () => {
    const anims = ['bubble', 'hum', 'stretch', 'yawn', 'happy_run', 'bubble'];
    const out = normalizeChoreo({ mood: '冒泡循环', steps: anims.map((anim) => step(anim)) })
      .steps.map((s) => s.anim);
    assertNoRepeatInLoop(out);
    assert.notEqual(out[0], out[out.length - 1]);
  });

  it('上一轮的最后一步也算第 0 步的邻居', () => {
    const out = normalizeChoreo(
      { mood: '接着上一轮', steps: [step('bubble'), step('hum')] },
      { recentAnims: ['bubble'] },
    ).steps.map((s) => s.anim);
    assert.notEqual(out[0], 'bubble', '第 0 步与上一轮末步重复却没被换掉');
    assert.notEqual(out[0], out[1]);
    for (const anim of out) assert.ok(ALL_ANIMS.includes(anim));
  });

  it('夜间防重复只用夜间池（yawn / stretch / hum / bubble 等）', () => {
    const anims = ['sleep', 'sleep', 'sleep', 'sleep'];
    const out = normalizeChoreo(
      { mood: '深夜犯困', steps: anims.map((anim) => step(anim)) },
      { night: true },
    ).steps.map((s) => s.anim);
    assertNoRepeatInLoop(out);
    for (const anim of out) {
      assert.ok(NIGHT_POOL.includes(anim), `${anim} 不在夜间池，夜里不该排它`);
    }
  });

  it('三段动作表（nva.cpp / proto.cpp / ai.js）逐项一致', () => {
    const root = new URL('../../', import.meta.url);
    const nvaPath = new URL('src/anim/nva.cpp', root);
    const protoPath = new URL('src/net/proto.cpp', root);
    // 插件被单独发布时仓库结构不在，跳过这条；在仓库内跑回归时必须生效。
    if (!existsSync(nvaPath) || !existsSync(protoPath)) return;

    const grab = (src, marker) => {
      const at = src.indexOf(marker);
      assert.ok(at >= 0, `找不到 ${marker}`);
      const body = src.slice(at, src.indexOf('};', at));
      return [...body.matchAll(/"([a-z_]+)"/g)].map((m) => m[1]);
    };

    assert.deepEqual(
      grab(readFileSync(nvaPath, 'utf8'), 'ANIM_NAMES['),
      ALL_ANIMS,
      'src/anim/nva.cpp 的 ANIM_NAMES 与契约不一致',
    );
    assert.deepEqual(
      grab(readFileSync(protoPath, 'utf8'), 'kAnimNames[] = {'),
      ALL_ANIMS,
      'src/net/proto.cpp 的 kAnimNames 与契约不一致',
    );
  });
});
