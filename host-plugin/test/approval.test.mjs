// 工具审批状态机：宿主侧 onApprovalRequest / settleApproval / handleUp 的契约测试。
// 全部用例断言「当前实现的行为」，不改业务代码即可跑绿，用来防止后续改动悄悄改变语义。
//
// 注意：标了 ⚠️ 的用例钉死的是**已知缺陷**，不是期望行为。
// 期望行为写在 ../../tools/approval.known-issues.mjs，手动跑会红，是待修清单：
//   node --test tools/approval.known-issues.mjs
//
// 跑法：cd host-plugin && npm test
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { createNovaBridge } from '../lib/bridge.js';

function createMockMqttClient() {
  const emitter = new EventEmitter();
  const published = [];
  return Object.assign(emitter, {
    connected: true,
    subscribed: [],
    published,
    subscribe(topics) {
      this.subscribed.push(topics);
    },
    publish(topic, payload, opts) {
      published.push({ topic, payload: JSON.parse(payload), opts });
    },
    end() {
      this.connected = false;
    },
  });
}

// fallbackMs 压到 20ms，让超时分支在单测里可跑；默认 prefer 才能测「等设备回答」。
function harness(overrides = {}) {
  const client = createMockMqttClient();
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
  return { client, bridge };
}

function online(client, ids) {
  for (const id of ids) {
    client.emit('message', `nova/${id}/hello`, Buffer.from(JSON.stringify({ online: true })));
  }
}

function up(client, id, data) {
  client.emit('message', `nova/${id}/up`, Buffer.from(JSON.stringify(data)));
}

function approvalMsgs(client) {
  return client.published.filter((p) => p.topic.endsWith('/approval'));
}

function endMsgs(client) {
  return client.published.filter((p) => p.topic.endsWith('/approval_end'));
}

function apIdOf(client) {
  return approvalMsgs(client).at(-1).payload.id;
}

// 让 next() 永远不自己先 resolve（prefer 模式下用不上，mirror 下测 local 优先时才需要）。
function nextSpy(returnValue = 'allowed-once') {
  const calls = [];
  return {
    calls,
    fn: async () => {
      calls.push(1);
      return returnValue;
    },
  };
}

describe('tool approval state machine (host side)', () => {
  it('publishes one approval per online device with id, ttl, qos1 and no retain', async () => {
    const { client, bridge } = harness({ fallbackMs: 1234 });
    try {
      online(client, ['D1', 'D2']);
      const spy = nextSpy();
      const pending = bridge.onApprovalRequest({ toolName: 'Bash', reason: 'rm -rf ./tmp' }, spy.fn);

      const msgs = approvalMsgs(client);
      assert.equal(msgs.length, 2);
      assert.deepEqual(msgs.map((m) => m.topic), ['nova/D1/approval', 'nova/D2/approval']);
      for (const m of msgs) {
        assert.equal(m.payload.t, 'approval');
        assert.match(m.payload.id, /^ap_/);
        assert.equal(m.payload.tool, 'Bash');
        assert.equal(m.payload.reason, 'rm -rf ./tmp');
        assert.equal(m.payload.ttl, 1234);
        assert.equal(m.opts.qos, 1);
        assert.equal(m.opts.retain, false);
      }

      up(client, 'D1', { evt: 'decision', id: apIdOf(client), accept: true });
      assert.equal(await pending, 'allowed-once');
    } finally {
      bridge.stop();
    }
  });

  // ⚠️ 已知缺陷：tool/reason 没走 sanitizeOneLine / shortenCommand，换行与 ANSI 会原样进 JSON。
  // 期望行为见 tools/approval.known-issues.mjs 的第 1 条。
  it('passes raw control characters through to the device (current behavior)', async () => {
    const { client, bridge } = harness();
    try {
      online(client, ['D1']);
      const reason = '会删 6 个录音\n\x1b[31m危险';
      const pending = bridge.onApprovalRequest({ toolName: 'Bash', reason }, nextSpy().fn);
      assert.equal(approvalMsgs(client).at(-1).payload.reason, reason);
      up(client, 'D1', { evt: 'decision', id: apIdOf(client), accept: false });
      await pending;
    } finally {
      bridge.stop();
    }
  });

  it('maps accept / deny / toPc decisions to their outcomes', async () => {
    for (const [decision, expected] of [
      [{ accept: true }, 'allowed-once'],
      [{ accept: false }, 'rejected'],
      [{ toPc: true }, 'allowed-once'],
    ]) {
      const { client, bridge } = harness();
      try {
        online(client, ['D1']);
        const spy = nextSpy();
        const pending = bridge.onApprovalRequest({ toolName: 'Bash' }, spy.fn);
        const id = apIdOf(client);
        up(client, 'D1', { evt: 'decision', id, ...decision });

        assert.equal(await pending, expected);
        // 只有 toPc（回退）才会调用电脑端对话框；设备直接答不调用。
        assert.equal(spy.calls.length, decision.toPc ? 1 : 0);
      } finally {
        bridge.stop();
      }
    }
  });

  it('rejects automatically when nobody answers within the TTL (prefer)', async () => {
    const { client, bridge } = harness({ fallbackMs: 20 });
    try {
      online(client, ['D1']);
      const spy = nextSpy();
      const outcome = await bridge.onApprovalRequest({ toolName: 'Bash' }, spy.fn);
      assert.equal(outcome, 'rejected');
      assert.equal(spy.calls.length, 0, '超时不应再打开电脑端对话框');
      const ends = endMsgs(client);
      assert.equal(ends.length, 1);
      assert.equal(ends[0].payload.id, apIdOf(client));
      assert.equal(ends[0].opts.qos, 1);
    } finally {
      bridge.stop();
    }
  });

  it('rejects automatically in mirror mode even while the desktop dialog is still open', async () => {
    const { client, bridge } = harness({ deviceApproval: 'mirror', fallbackMs: 20 });
    try {
      online(client, ['D1']);
      // next() 永不 settle，模拟电脑端对话框一直没人点。
      const outcome = await bridge.onApprovalRequest(
        { toolName: 'Bash' },
        () => new Promise(() => {}),
      );
      assert.equal(outcome, 'rejected');
      assert.equal(endMsgs(client).length, 1);
    } finally {
      bridge.stop();
    }
  });

  it('settles only the first decision: duplicates and late decisions are ignored', async () => {
    const { client, bridge } = harness();
    try {
      online(client, ['D1']);
      const spy = nextSpy();
      const pending = bridge.onApprovalRequest({ toolName: 'Bash' }, spy.fn);
      const id = apIdOf(client);

      up(client, 'D1', { evt: 'decision', id, accept: true });
      up(client, 'D1', { evt: 'decision', id, accept: false });
      assert.equal(await pending, 'allowed-once');

      // 结算之后再发：pendingApprovals 里已无此 id，不能抛异常也不能再调 next()。
      up(client, 'D1', { evt: 'decision', id, accept: false });
      assert.equal(spy.calls.length, 0);
    } finally {
      bridge.stop();
    }
  });

  it('abort rejects and notifies devices; stop() drains all pending approvals', async () => {
    {
      const { client, bridge } = harness();
      try {
        online(client, ['D1']);
        const ac = new AbortController();
        const pending = bridge.onApprovalRequest({ toolName: 'Bash', signal: ac.signal }, nextSpy().fn);
        ac.abort();
        assert.equal(await pending, 'rejected');
        assert.equal(endMsgs(client).length, 1);
      } finally {
        bridge.stop();
      }
    }
    {
      const { client, bridge } = harness();
      online(client, ['D1']);
      const pending = bridge.onApprovalRequest({ toolName: 'Bash' }, nextSpy().fn);
      bridge.stop();
      assert.equal(await pending, 'rejected');
      assert.equal(endMsgs(client).length, 1);
    }
  });

  it('mirror mode lets the desktop dialog win and broadcasts approval_end to every device', async () => {
    const { client, bridge } = harness({ deviceApproval: 'mirror' });
    try {
      online(client, ['D1', 'D2']);
      const spy = nextSpy();
      assert.equal(await bridge.onApprovalRequest({ toolName: 'Bash' }, spy.fn), 'allowed-once');
      assert.equal(spy.calls.length, 1);
      assert.deepEqual(endMsgs(client).map((m) => m.topic).sort(), [
        'nova/D1/approval_end',
        'nova/D2/approval_end',
      ]);
    } finally {
      bridge.stop();
    }
  });

  // ⚠️ 已知缺陷：bridge.js:562-566 设备决策时 notifyDeviceEnd:false，
  // 另一台设备的弹窗只能靠自己的 TTL 自毁，会滞留最多 20 秒。
  it('does not clear other devices when one device decides (current behavior)', async () => {
    const { client, bridge } = harness();
    try {
      online(client, ['D1', 'D2']);
      const pending = bridge.onApprovalRequest({ toolName: 'Bash' }, nextSpy().fn);
      up(client, 'D1', { evt: 'decision', id: apIdOf(client), accept: true });
      assert.equal(await pending, 'allowed-once');
      assert.equal(endMsgs(client).length, 0);
    } finally {
      bridge.stop();
    }
  });

  // ⚠️ 已知缺陷：handleUp 只比对 apId，不校验来源设备是否在 entry.deviceIds 里。
  it('accepts a decision from a device that was never sent the approval (current behavior)', async () => {
    const { client, bridge } = harness();
    try {
      online(client, ['D1']);
      const pending = bridge.onApprovalRequest({ toolName: 'Bash' }, nextSpy().fn);
      const id = apIdOf(client);
      assert.equal(approvalMsgs(client).length, 1);
      up(client, 'D9', { evt: 'decision', id, accept: true });
      assert.equal(await pending, 'allowed-once');
    } finally {
      bridge.stop();
    }
  });

  it('skips the device entirely when disabled, offline or disconnected', async () => {
    {
      const { client, bridge } = harness({ deviceApproval: 'off' });
      try {
        online(client, ['D1']);
        assert.equal(await bridge.onApprovalRequest({}, nextSpy().fn), 'allowed-once');
        assert.equal(approvalMsgs(client).length, 0);
      } finally {
        bridge.stop();
      }
    }
    {
      const { client, bridge } = harness();
      try {
        assert.equal(await bridge.onApprovalRequest({}, nextSpy().fn), 'allowed-once');
        assert.equal(approvalMsgs(client).length, 0);
      } finally {
        bridge.stop();
      }
    }
    {
      const { client, bridge } = harness();
      try {
        online(client, ['D1']);
        client.connected = false;
        assert.equal(await bridge.onApprovalRequest({}, nextSpy().fn), 'allowed-once');
        assert.equal(approvalMsgs(client).length, 0);
      } finally {
        bridge.stop();
      }
    }
  });
});
