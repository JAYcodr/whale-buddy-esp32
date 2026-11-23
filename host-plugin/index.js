import { createNovaBridge } from './lib/bridge.js';

export const name = 'dsh-plugin-nova-buddy';

/**
 * cordis 的服务属性不是普通字段：没在 inject 里声明的服务名，一旦用
 * `ctx.<name>` 取就会抛 `cannot get property "<name>" without inject`。
 * 这个异常发生在 `session/event` 派发时，被 invokeContainedSessionObservers
 * 吞成一条 warn 日志，不中断派发——于是监听器看起来"注册成功、事件也到了"，
 * 实际回调体在第一行就炸掉。
 *
 * `tokenMeter` 由 dsh-base 的 token-meter 条目提供，始终存在，故按必需依赖声明。
 */
export const inject = ['tokenMeter'];

export function apply(ctx, config = {}) {
  const logger = ctx.logger?.('nova-buddy');
  const log = (msg) => logger?.info?.(msg);
  // ctx.logger 不一定有 warn/error；没有就逐级降级，别把日志本身变成异常源。
  log.warn = (msg) => (logger?.warn ? logger.warn(msg) : logger?.info?.(msg));
  log.error = (msg) => (logger?.error ? logger.error(msg)
                     : logger?.warn ? logger.warn(msg)
                     : logger?.info?.(msg));

  const bridge = createNovaBridge({
    brokerUrl: config.brokerUrl,     // 不设默认值：undefined 会自然落到 env 和 bridge 内的默认
    deviceApproval: config.deviceApproval || 'mirror',
    fallbackMs: config.fallbackMs ?? 20000,
    ttsVoice: config.ttsVoice,
    ttsStyle: config.ttsStyle,
    quietHours: config.quietHours,
    choreoIntervalMs: config.choreoIntervalMs,
    tokensEnabled: config.tokens !== false,
    log,
  });

  // 服务访问放在派发路径之外求值一次：即便 inject 声明失效，也只会拿到
  // undefined（readCtxPct 有兜底，只是不显示上下文占比），不会把整个
  // session/event 回调炸掉。
  let tokenMeter;
  try {
    tokenMeter = ctx.tokenMeter;
  } catch {
    tokenMeter = undefined;
  }

  bridge.start();

  ctx.effect(() => {
    // 这些事件都按 agent scope 路由（scopeTarget），不带 global 的监听只能收到
    // 本插件自己作用域内的事件——而本插件不持有任何 session，等于永远收不到。
    // 必须 global: true 才能看到所有会话；审批是 waterfall，还要 prepend 抢占链头。
    const disposers = [
      ctx.on('session/event', (session, event) => bridge.onSessionEvent(session, event, tokenMeter), {
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
