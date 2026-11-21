import { createNovaBridge } from './lib/bridge.js';

export function apply(ctx, config = {}) {
  const bridge = createNovaBridge({
    brokerUrl: config.brokerUrl || 'mqtt://192.168.31.15:1883',
    deviceApproval: config.deviceApproval || 'prefer',
    fallbackMs: config.fallbackMs ?? 8000,
    ttsVoice: config.ttsVoice,
    ttsStyle: config.ttsStyle,
    quietHours: config.quietHours,
    choreoIntervalMs: config.choreoIntervalMs,
    log: (msg) => ctx.logger?.('nova-buddy')?.info?.(msg),
  });

  bridge.start();

  const disposers = [
    ctx.on('session/event', (session, event) => bridge.onSessionEvent(session, event)),
    ctx.on('session/disposed', (session) => bridge.onSessionDisposed(session)),
    ctx.on('approval/request', (request, next) => bridge.onApprovalRequest(request, next), {
      prepend: true,
    }),
  ];

  ctx.on('dispose', () => {
    for (const dispose of disposers) {
      try {
        dispose?.();
      } catch {
        /* ignore */
      }
    }
    bridge.stop();
  });
}
