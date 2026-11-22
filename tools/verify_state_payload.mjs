#!/usr/bin/env node
// 用真实的 DSH 事件形状驱动 bridge，确认 state 载荷全链路。
//
// 事件形状取自 DSH 的 SessionEventMap（app.asar 导出的 .d.ts）：
//   'turn/end': { turn: number; reason: TurnEndReason }
//   TurnEndReasonMap.error = { kind: 'error'; error: LlmFailure }
//   LlmFailure = { message: string; code: string; status?: number; requestId?: ... }
//
// 不需要真机、不需要 broker 连上（createNovaBridge 在未 connect 时也能算载荷）。
// 用法：node tools/verify_state_payload.mjs
import { createNovaBridge } from '../host-plugin/lib/bridge.js';

const b = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883' });
b.onSessionEvent({ id: 's1' }, { type: 'session/title', data: { title: 'issue 核查与防烧屏界面烧录', messageSeqs: [], source: 'auto' } });
b.onSessionEvent({ id: 's1' }, { type: 'turn/start', data: { turn: 1 } });
b.onSessionEvent({ id: 's1' }, { type: 'tool/call', data: { turn: 1, step: 1, callId: 'c1', name: 'bash', arguments: '{"command":"pio run"}' } });

// 真实 TurnEndReasonMap 的 error 分支：{ kind:'error', error: LlmFailure }
b.onSessionEvent({ id: 's1' }, {
  type: 'turn/end',
  data: { turn: 1, reason: { kind: 'error', error: {
    message: 'HTTP 429: rate limited', code: 'rate_limited', status: 429, requestId: 'req_9f3a2c',
  } } },
});

const st = b.buildStatePayload();
console.log('payload:', JSON.stringify(st, null, 2));
console.log('\n--- 断言 ---');
const checks = [
  ['err 是布尔', typeof st.err === 'boolean'],
  ['err === true', st.err === true],
  ['errMsg 有内容', st.errMsg === 'HTTP 429: rate limited'],
  ['msg 带原文', st.msg === '本轮异常结束 · HTTP 429: rate limited'],
  ['title 保留', st.title === 'issue 核查与防烧屏界面烧录'],
  ['tool 保留', st.tool === 'bash'],
];
let bad = 0;
for (const [n, ok] of checks) { console.log((ok ? '✓' : '✗'), n); if (!ok) bad++; }
b.stop();
process.exit(bad ? 1 : 0);
