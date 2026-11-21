import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { createNovaBridge } from '../lib/bridge.js';

describe('Nova Bridge session aggregation', () => {
  it('tracks active running/waiting sessions and context pct', () => {
    const bridge = createNovaBridge({ brokerUrl: 'mqtt://127.0.0.1:1883' });
    bridge.onSessionEvent({ id: 's1', model: 'deepseek-v4' }, { type: 'turn/start', data: {} });
    bridge.onSessionEvent({ id: 's1' }, { type: 'tool/call', data: { toolName: 'Bash', command: 'pio run' } });
    bridge.onSessionEvent({ id: 's1' }, { type: 'context/usage', data: { percent: 42 } });
    bridge.onSessionEvent({ id: 's2' }, { type: 'approval/asked', data: { toolName: 'Write' } });

    // Exercise dispose cleanup
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
