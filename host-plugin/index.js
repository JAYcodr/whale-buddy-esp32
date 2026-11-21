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
    // 这些事件都按 agent scope 路由（scopeTarget），不带 global 的监听只能收到
    // 本插件自己作用域内的事件——而本插件不持有任何 session，等于永远收不到。
    // 必须 global: true 才能看到所有会话；审批是 waterfall，还要 prepend 抢占链头。
    const disposers = [
      ctx.on('session/event', (session, event) => bridge.onSessionEvent(session, event, ctx.tokenMeter), {
        global: true,
      }),
      ctx.on('session/disposed', (session) => bridge.onSessionDisposed(session), { global: true }),
      ctx.on('approval/request', (request, next) => bridge.onApprovalRequest(request, next), {
        global: true,
        prepend: true,
      }),
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
