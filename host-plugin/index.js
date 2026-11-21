import { createNovaBridge } from './lib/bridge.js';

export const name = 'dsh-plugin-nova-buddy';

export function apply(ctx, config = {}) {
  const logger = ctx.logger?.('nova-buddy');
  const log = (msg) => logger?.info?.(msg);

  const bridge = createNovaBridge({
    brokerUrl: config.brokerUrl || 'mqtt://192.168.31.15:1883',
    deviceApproval: config.deviceApproval || 'prefer',
    fallbackMs: config.fallbackMs ?? 8000,
    ttsVoice: config.ttsVoice,
    ttsStyle: config.ttsStyle,
    quietHours: config.quietHours,
    choreoIntervalMs: config.choreoIntervalMs,
    log,
  });

  bridge.start();

  ctx.effect(() => {
    const disposers = [
      ctx.on('session/event', (session, event) => bridge.onSessionEvent(session, event, ctx.tokenMeter)),
      ctx.on('session/disposed', (session) => bridge.onSessionDisposed(session)),
      ctx.on('approval/request', (request, next) => bridge.onApprovalRequest(request, next)),
    ];
    return () => {
      for (const dispose of disposers) {
        try {
          dispose?.();
        } catch {
          /* ignore */
        }
      }
      bridge.stop();
    };
  }, 'nova-buddy: MQTT bridge');
}
