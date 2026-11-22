import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { zstdCompressSync } from 'node:zlib';
import { createTokenCollector } from '../lib/tokens.js';

/** 本地时间构造，避免测试受运行机器时区影响。 */
const at = (y, m, d) => new Date(y, m - 1, d, 12, 0, 0).getTime();

const eventLine = (type, turn, step, time, usage) =>
  JSON.stringify({ type, time, data: { turn, step, usage } }) + '\n';

/** DSH 的日志是逐条 append 的 zstd 多帧流 —— 造数据时必须真的按帧拼接。 */
const frame = (text) => zstdCompressSync(Buffer.from(text, 'utf8'));

async function makeTree(lines) {
  const home = await mkdtemp(join(tmpdir(), 'nova-tokens-'));
  const sessionDir = join(home, 'sessions', 'scope-a', 'session-1');
  await mkdir(sessionDir, { recursive: true });
  // lines 是帧数组：每个元素单独压一帧再拼起来
  await writeFile(join(sessionDir, 'session.v4.jsonl.zstd'), Buffer.concat(lines.map(frame)));
  return home;
}

test('token collector folds multi-frame session logs into a device payload', async () => {
  const home = await makeTree([
    eventLine('assistant/message', 1, 1, at(2026, 11, 20), {
      inputTokens: 1_000, outputTokens: 10, cacheReadTokens: 9_000, cacheWriteTokens: 0,
    }),
    eventLine('assistant/message', 1, 2, at(2026, 11, 21), {
      inputTokens: 2_000, outputTokens: 20, cacheReadTokens: 18_000, cacheWriteTokens: 0,
    }),
  ]);

  const collector = createTokenCollector({ dshHome: home, log: () => {} });
  const payload = await collector.refresh(true);

  assert.equal(payload.t, 'tokens');
  assert.equal(payload.spanDays, 2, '两天各一条');
  assert.deepEqual(payload.days, [
    ['11/20', 10_010],
    ['11/21', 20_020],
  ]);
  assert.equal(payload.total, 30_030);
  // 命中率是输入侧：27000 / (27000 + 3000)
  assert.equal(payload.cachePct, 90);
  // avg 交给设备端按 days 自己算，host 不再重复实现
  assert.equal(payload.avg, undefined);
});

test('same turn/step replaces, retries do not double count', async () => {
  const usage = { inputTokens: 500, outputTokens: 5, cacheReadTokens: 4_500, cacheWriteTokens: 0 };
  const retry = { inputTokens: 700, outputTokens: 7, cacheReadTokens: 6_300, cacheWriteTokens: 0 };
  const home = await makeTree([
    eventLine('assistant/message', 1, 1, at(2026, 11, 21), usage),
    // 同一步骤的重试样本：应替换而不是相加
    eventLine('assistant/attempt', 1, 1, at(2026, 11, 21), retry),
  ]);

  const payload = await createTokenCollector({ dshHome: home, log: () => {} }).refresh(true);
  assert.equal(payload.total, 7_007);
  assert.equal(payload.days.length, 1);
});

test('days keeps the most recent 14 and spanDays counts every day', async () => {
  const lines = [];
  for (let i = 1; i <= 17; i += 1) {
    lines.push(
      eventLine('assistant/message', i, 1, at(2026, 10, i), {
        inputTokens: i, outputTokens: 0, cacheReadTokens: 0, cacheWriteTokens: 0,
      }),
    );
  }
  const home = await makeTree(lines);
  const payload = await createTokenCollector({ dshHome: home, log: () => {} }).refresh(true);

  assert.equal(payload.spanDays, 17);
  assert.equal(payload.days.length, 14);
  assert.equal(payload.days[payload.days.length - 1][0], '10/17', '最新一天在最右');
  assert.equal(payload.days[0][0], '10/04');
});

test('cache is reused so a second refresh skips unchanged files', async () => {
  const home = await makeTree([
    eventLine('assistant/message', 1, 1, at(2026, 11, 21), {
      inputTokens: 10, outputTokens: 1, cacheReadTokens: 90, cacheWriteTokens: 0,
    }),
  ]);
  const collector = createTokenCollector({ dshHome: home, log: () => {} });
  await collector.refresh(true);
  const again = await collector.refresh(false);
  assert.equal(again.total, 101);
});
