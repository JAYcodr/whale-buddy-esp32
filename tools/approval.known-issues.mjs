#!/usr/bin/env node
// 工具审批链路「已知缺陷」的期望行为测试 —— 故意不进 npm test 主回归。
//
// 跑法（预期全红，红出来的就是待修清单）：
//   node --test tools/approval.known-issues.mjs
//
// 配套：host-plugin/test/approval.test.mjs 钉死的是当前行为（全绿）。
// 每修好一个缺陷，就把这里的期望断言搬进那个文件并把 ⚠️ 用例改过来。
// 放在 tools/ 而不是 host-plugin/test/，是为了不被 `node --test` 自动收集。
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { createNovaBridge } from '../host-plugin/lib/bridge.js';

function mockClient() {
  const emitter = new EventEmitter();
  const published = [];
  return Object.assign(emitter, {
    connected: true,
    published,
    subscribe() {},
    publish(topic, payload, opts) {
      published.push({ topic, payload: JSON.parse(payload), opts });
    },
    end() {
      this.connected = false;
    },
  });
}

function harness(overrides = {}) {
  const client = mockClient();
  const bridge = createNovaBridge({
    brokerUrl: 'mqtt://127.0.0.1:1883',
    mqttConnect: () => client,
    fallbackMs: 20,
    deviceApproval: 'prefer',
    powerLogEnabled: false,
    tokensEnabled: false,
    synthesizeSpeech: async () => Buffer.alloc(64, 1),
    choreoGenerator: async () => null,
    fetchAllQuota: async () => null,
    fetchModelDialRank: async () => null,
    ...overrides,
  });
  bridge.start();
  client.emit('message', 'nova/D1/hello', Buffer.from(JSON.stringify({ online: true })));
  return { client, bridge };
}

const approvals = (client) => client.published.filter((p) => p.topic.endsWith('/approval'));
const ends = (client) => client.published.filter((p) => p.topic.endsWith('/approval_end'));
const apIdOf = (client) => approvals(client).at(-1).payload.id;
const up = (client, id, data) =>
  client.emit('message', `nova/${id}/up`, Buffer.from(JSON.stringify(data)));

describe('approval: expected behavior (known issues, intentionally red)', () => {
  // 缺陷 1：bridge.js:1193-1196 没走 sanitizeOneLine / shortenCommand。
  it('strips newlines and ANSI escapes before showing a command on the device', async () => {
    const { client, bridge } = harness();
    try {
      const pending = bridge.onApprovalRequest(
        { toolName: 'Bash', reason: '会删 6 个录音\n\x1b[31m危险' },
        async () => 'desktop-dialog',
      );
      const reason = approvals(client).at(-1).payload.reason;
      assert.ok(!reason.includes('\n') && !reason.includes('\x1b'), 'reason 不应含控制字符');
      up(client, 'D1', { evt: 'decision', id: apIdOf(client), accept: false });
      await pending;
    } finally {
      bridge.stop();
    }
  });

  // 缺陷 2（超时自动拒绝）已修复，断言已搬进 host-plugin/test/approval.test.mjs。

  // 缺陷 3：bridge.js:562-566 设备决策 notifyDeviceEnd:false，其它设备弹窗滞留。
  it('clears the popup on every device after any one of them decides', async () => {
    const { client, bridge } = harness();
    try {
      client.emit('message', 'nova/D2/hello', Buffer.from(JSON.stringify({ online: true })));
      const pending = bridge.onApprovalRequest({ toolName: 'Bash' }, async () => 'desktop-dialog');
      up(client, 'D1', { evt: 'decision', id: apIdOf(client), accept: true });
      await pending;
      assert.ok(
        ends(client).some((m) => m.topic === 'nova/D2/approval_end'),
        'D2 也应收到 approval_end',
      );
    } finally {
      bridge.stop();
    }
  });

  // 缺陷 4：handleUp 只比对 apId，不校验来源设备属于本次审批的 deviceIds。
  it('ignores a decision coming from a device that never received the approval', async () => {
    const { client, bridge } = harness();
    try {
      const pending = bridge.onApprovalRequest({ toolName: 'Bash' }, async () => 'desktop-dialog');
      up(client, 'D9', { evt: 'decision', id: apIdOf(client), accept: true });
      assert.equal(await pending, 'desktop-dialog', '越权 decision 不应放行危险工具');
    } finally {
      bridge.stop();
    }
  });

  // 缺陷 5：bridge.js:990-992 的 offline/reconnect 只打日志，pendingApprovals 不清场；
  // 叠加 net.cpp:326 的 cleanSession=false，重连后旧 approval 会被重放。
  it('clears pending approvals as soon as the broker connection drops', async () => {
    const { client, bridge } = harness();
    try {
      const pending = bridge.onApprovalRequest({ toolName: 'Bash' }, async () => 'desktop-dialog');
      client.emit('offline');
      assert.ok(ends(client).length > 0, '掉线时应立刻发 approval_end 清场');
      up(client, 'D1', { evt: 'decision', id: apIdOf(client), accept: false });
      await pending;
    } finally {
      bridge.stop();
    }
  });
});
