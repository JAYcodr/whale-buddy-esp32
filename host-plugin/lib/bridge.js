import mqtt from 'mqtt';
import { fetchAllQuota, fetchModelDialRank } from './fetchers.js';
import { generateChoreography, loadAiConfig, synthesizeSpeechAdpcm } from './ai.js';
import { createTokenCollector } from './tokens.js';

const TTS_CHUNK_BYTES = 1800;

/** 从 tool/call 的 arguments 里摘一段人类可读预览（arguments 是 JSON 字符串）。 */
function toolArgPreview(raw) {
  if (raw == null) return '';
  if (typeof raw === 'string') {
    try {
      return toolArgPreview(JSON.parse(raw));
    } catch {
      return raw.slice(0, 60);
    }
  }
  if (typeof raw !== 'object') return String(raw).slice(0, 60);
  for (const key of ['command', 'path', 'file_path', 'query', 'pattern', 'url', 'description', 'prompt']) {
    const v = raw[key];
    if (typeof v === 'string' && v.trim()) return v.trim();
  }
  const firstString = Object.values(raw).find((v) => typeof v === 'string' && v.trim());
  return typeof firstString === 'string' ? firstString : '';
}

/** 上下文占用百分比：tokenMeter.contextPressure() → { pressureTokens, contextWindow }。 */
function readCtxPct(session, tokenMeter) {
  if (!session || !tokenMeter || typeof tokenMeter.contextPressure !== 'function') return null;
  try {
    const { pressureTokens, contextWindow } = tokenMeter.contextPressure(session) || {};
    if (!pressureTokens || !contextWindow) return null;
    return Math.max(0, Math.min(100, Math.round((pressureTokens / contextWindow) * 100)));
  } catch {
    return null;
  }
}

/**
 * 播报文本截断：设备侧单条 TTS 上限 MAX_TTS_BYTES=98304（4bit@16kHz ≈ 8000B/s），
 * 约 12.3 秒语音 ≈ 43–55 个汉字。超了 applyTtsChunk 会 free + return false，
 * 整条语音静默丢掉 —— 所以必须在送进合成之前就压到 ~40 字。
 */
const TTS_MAX_CHARS = 40;
function clipForTts(text, limit = TTS_MAX_CHARS) {
  const s = String(text || '').replace(/\s+/g, ' ').trim();
  if (!s) return '';
  return s.length <= limit ? s : `${s.slice(0, limit - 1)}…`;
}

export function createNovaBridge(options = {}) {
  const brokerUrl = options.brokerUrl || process.env.NOVA_MQTT_URL || 'mqtt://192.168.31.15:1883';
  const fallbackMs = Number(options.fallbackMs ?? 8000);
  const deviceApproval = options.deviceApproval || 'prefer'; // 'prefer' | 'mirror' | 'off'
  const log = options.log || (() => {});
  const aiCfg = loadAiConfig(options);
  const ttsSynth = options.synthesizeSpeech || synthesizeSpeechAdpcm;
  const quotaFetcher = options.fetchAllQuota || fetchAllQuota;
  const rankFetcher = options.fetchModelDialRank || fetchModelDialRank;
  const mqttConnectFn = options.mqttConnect || ((url, opts) => mqtt.connect(url, opts));
  // token 页的数据源是会话日志而不是 ctx 服务：`ctx.sessions` 只列 live session，
  // 拿不到历史，而累计页要的正是历史。见 lib/tokens.js 顶部注释。
  const tokenCollector = createTokenCollector({
    log,
    enabled: options.tokensEnabled !== false,
    dshHome: options.dshHome,
  });

  const devices = new Map(); // id -> { online, lastSeen, meta, prefs, stat }
  const sessions = new Map(); // sessionId -> { running, waiting, error, lastTool, msg, ctxPct, model, updatedAt }
  const pendingApprovals = new Map(); // apId -> { resolve, deviceIds, timer }

  let client = null;
  let quotaPayload = null;
  let rankPayload = null;
  let choreoPayload = null;
  let tokensPayload = null;
  let recentAnims = [];
  let recentMood = '';
  let lastStateJson = '';
  let lastStateSentAt = 0;
  let lastChoreoAt = 0;
  let lastTtsAt = 0;
  // force 一开节流就形同虚设，必须补去重：同一条错误连播会把语音串成一片。
  const ttsDedup = new Map();  // key -> lastPlayedMs
  const TTS_DEDUP_MS = 30000;

  function ttsDedupHit(key) {
    const now = Date.now();
    const last = ttsDedup.get(key);
    if (last && now - last < TTS_DEDUP_MS) return true;
    ttsDedup.set(key, now);
    // 顺手清掉过期条目，防止无限增长。
    for (const [k, t] of ttsDedup) {
      if (now - t >= TTS_DEDUP_MS) ttsDedup.delete(k);
    }
    return false;
  }
  let lastSyncQuotaAt = 0;
  let ttsSeqId = 1;
  let stateTimer = null;
  let quotaTimer = null;
  let rankTimer = null;
  let hbTimer = null;
  let choreoTimer = null;
  let tokensTimer = null;
  let closed = false;

  function onlineIds() {
    const out = [];
    for (const [id, info] of devices.entries()) {
      if (info.online) out.push(id);
    }
    return out;
  }

  function ttsTargetIds() {
    const out = [];
    for (const [id, info] of devices.entries()) {
      if (!info.online) continue;
      const p = info.prefs;
      if (p && (p.tts === false || p.snd === 0)) continue;
      out.push(id);
    }
    return out;
  }

  function anyQuietOverride(ids) {
    return ids.some((id) => devices.get(id)?.prefs?.quiet === false);
  }

  function publishTo(id, suffix, obj, { qos = 1, retain = false } = {}) {
    if (!client?.connected) return;
    client.publish(`nova/${id}/${suffix}`, JSON.stringify(obj), { qos, retain });
  }

  function publishAll(suffix, obj, opts) {
    for (const id of onlineIds()) {
      publishTo(id, suffix, obj, opts);
    }
  }

  function buildStatePayload() {
    let run = 0;
    let wait = 0;
    let chosen = null;
    let bestPrio = -1;
    let latestModel = '';
    let latestModelAt = -1;
    let latestCtxPct;
    let latestCtxAt = -1;

    for (const s of sessions.values()) {
      if (s.running) run++;
      if (s.waiting) wait++;
      const prio = s.waiting ? 3 : (s.running ? 2 : (s.error ? 1 : 0));
      if (!chosen || prio > bestPrio || (prio === bestPrio && s.updatedAt >= chosen.updatedAt)) {
        chosen = s;
        bestPrio = prio;
      }
      if (s.model && s.updatedAt >= latestModelAt) {
        latestModel = s.model;
        latestModelAt = s.updatedAt;
      }
      if (s.ctxPct != null && s.updatedAt >= latestCtxAt) {
        latestCtxPct = s.ctxPct;
        latestCtxAt = s.updatedAt;
      }
    }

    const latestMsg = chosen?.msg || (run > 0 ? '正在处理...' : '待命中');
    const model = chosen?.model || latestModel;
    const ctxPct = chosen?.ctxPct != null ? chosen.ctxPct : latestCtxPct;

    const payload = {
      t: 'state',
      run,
      wait,
      sess: sessions.size,
      msg: latestMsg,
    };
    // err 保持 bool 不动：固件是 `doc["err"] | false`，ArduinoJson 对数字/字符串/对象
    // 一律求值 0，只有字面量 true 能活。所以异常原文走平级字段，别塞进 err。
    if (run === 0 && chosen?.error) payload.err = true;
    if (ctxPct != null) payload.ctx = { pct: Math.max(0, Math.min(100, Math.round(ctxPct))) };
    if (model) payload.model = model;
    // 码头要用的信息：当前这条会话叫什么、上一个工具是什么、异常原文。
    if (chosen?.title) payload.title = chosen.title;
    if (chosen?.lastTool) payload.tool = chosen.lastTool;
    if (chosen?.error && chosen.errMsg) payload.errMsg = chosen.errMsg;
    return payload;
  }

  function flushState(force = false) {
    const payload = buildStatePayload();
    const raw = JSON.stringify(payload);
    const now = Date.now();
    if (!force && raw === lastStateJson && now - lastStateSentAt < 5000) return;
    if (!force && now - lastStateSentAt < 400) {
      if (!stateTimer) {
        stateTimer = setTimeout(() => {
          stateTimer = null;
          flushState(true);
        }, 400);
      }
      return;
    }
    lastStateJson = raw;
    lastStateSentAt = now;
    publishAll('state', payload, { qos: 1, retain: true });
  }

  function pushTime(id) {
    const epoch = Math.floor(Date.now() / 1000);
    const tz = -new Date().getTimezoneOffset() * 60;
    publishTo(id, 'time', { t: 'time', epoch, tz }, { qos: 0, retain: true });
  }

  async function refreshQuota() {
    quotaPayload = await quotaFetcher(options.credentials);
    publishAll('quota', quotaPayload, { qos: 1, retain: true });
  }

  async function refreshRank(force = false) {
    const r = await rankFetcher(force);
    if (r) {
      rankPayload = r;
      publishAll('rank', rankPayload, { qos: 1, retain: true });
    }
  }

  async function refreshTokens(force = false) {
    const next = await tokenCollector.refresh(force);
    if (!next) return null;
    // 内容没变就别刷 retain：设备端 tokenGen 每次都自增会白重绘一页。
    const changed = JSON.stringify(next) !== JSON.stringify(tokensPayload);
    tokensPayload = next;
    if (changed) publishAll('tokens', tokensPayload, { qos: 1, retain: true });
    return tokensPayload;
  }

  function buildContextSummary(reason = 'timer') {
    const st = buildStatePayload();
    let ds = '';
    const cards = quotaPayload?.cards || [];
    const dsCard = cards.find((c) => c.id === 'ds');
    if (dsCard?.kind === 'bal') ds = `DeepSeek余额${dsCard.val}元`;
    const ocCard = cards.find((c) => c.id === 'oc');
    const ocPct = ocCard?.bars?.[0]?.pct;
    const ocInfo = ocPct != null ? `，OpenCode本周已用${ocPct}%` : '';
    const modInfo = st.model ? `，模型=${st.model}` : '';
    return `触发=${reason}，DSH运行=${st.run}，等待=${st.wait}，会话=${st.sess}${modInfo}，${ds}${ocInfo}`;
  }

  async function refreshChoreo(reason = 'timer', minCooldownMs = 0) {
    const now = Date.now();
    if (minCooldownMs > 0 && now - lastChoreoAt < minCooldownMs) return null;
    if (!aiCfg.llm?.primary?.apiKey && !aiCfg.llm?.fallback?.apiKey) return null;
    lastChoreoAt = now;
    const c = await generateChoreography(buildContextSummary(reason), aiCfg, log, { recentAnims, recentMood });
    if (c) {
      choreoPayload = c;
      recentMood = c.mood || '';
      recentAnims = c.steps.map((s) => s.anim);
      publishAll('choreo', choreoPayload, { qos: 1, retain: true });
    }
    return c;
  }

  async function publishTts(text, opts = {}) {
    const now = Date.now();
    if (!opts.force && now - lastTtsAt < 4000) return false;
    const targets = ttsTargetIds();
    if (targets.length === 0 || !client?.connected) return false;
    const synthOpts = {
      ...opts,
      ignoreQuiet: Boolean(opts.ignoreQuiet || anyQuietOverride(targets)),
    };
    try {
      const adp = await ttsSynth(text, synthOpts, aiCfg);
      if (!adp || adp.length <= 16) return false;
      lastTtsAt = Date.now();
      const clipId = ttsSeqId++;
      const total = Math.ceil(adp.length / TTS_CHUNK_BYTES);
      for (const id of targets) {
        for (let seq = 0; seq < total; seq++) {
          const slice = adp.subarray(seq * TTS_CHUNK_BYTES, (seq + 1) * TTS_CHUNK_BYTES);
          publishTo(
            id,
            'tts',
            { t: 'tts', id: clipId, seq, tot: total, b64: slice.toString('base64') },
            { qos: 1, retain: false },
          );
        }
      }
      log(`tts pushed clip=${clipId} bytes=${adp.length} chunks=${total} targets=${targets.length}`);
      return true;
    } catch (err) {
      log(`tts error: ${err.message}`);
      return false;
    }
  }

  function settleApproval(apId, outcome, { notifyDeviceEnd = false } = {}) {
    const entry = pendingApprovals.get(apId);
    if (!entry) return;
    pendingApprovals.delete(apId);
    if (entry.timer) clearTimeout(entry.timer);
    if (notifyDeviceEnd) {
      for (const id of entry.deviceIds) {
        publishTo(id, 'approval_end', { t: 'approval_end', id: apId }, { qos: 1, retain: false });
      }
    }
    entry.resolve(outcome);
  }

  function handleHello(id, data) {
    const online = data?.online !== false;
    const prev = devices.get(id) || {};
    const prefs = data?.prefs && typeof data.prefs === 'object' ? data.prefs : prev.prefs || null;
    devices.set(id, { ...prev, online, lastSeen: Date.now(), meta: data?.hello || prev.meta || null, prefs });
    log(`device ${id} ${online ? 'online' : 'offline'}`);
    if (online) {
      pushTime(id);
      publishTo(id, 'state', buildStatePayload(), { qos: 1, retain: true });
      if (quotaPayload) publishTo(id, 'quota', quotaPayload, { qos: 1, retain: true });
      if (rankPayload) publishTo(id, 'rank', rankPayload, { qos: 1, retain: true });
      if (tokensPayload) publishTo(id, 'tokens', tokensPayload, { qos: 1, retain: true });
      if (choreoPayload && prefs?.choreo !== false) {
        publishTo(id, 'choreo', choreoPayload, { qos: 1, retain: true });
      }
    }
  }

  function handleUp(id, data) {
    if (data?.evt === 'decision' && data.id) {
      const apId = String(data.id);
      if (data.toPc) {
        settleApproval(apId, { kind: 'fallback' }, { notifyDeviceEnd: false });
      } else {
        settleApproval(
          apId,
          { kind: 'decided', outcome: data.accept ? 'allowed-once' : 'rejected' },
          { notifyDeviceEnd: false },
        );
      }
    } else if (data?.evt === 'stat') {
      devices.set(id, { ...(devices.get(id) || {}), online: true, lastSeen: Date.now(), stat: data });
    } else if (data?.evt === 'prefs') {
      const prev = devices.get(id) || {};
      const prefs = {
        snd: data.snd ?? prev.prefs?.snd ?? 75,
        tts: data.tts ?? prev.prefs?.tts ?? true,
        quiet: data.quiet ?? prev.prefs?.quiet ?? true,
        choreo: data.choreo ?? prev.prefs?.choreo ?? true,
      };
      devices.set(id, { ...prev, online: true, lastSeen: Date.now(), prefs });
      log(`device ${id} prefs updated: ${JSON.stringify(prefs)}`);
    } else if (data?.evt === 'sync') {
      devices.set(id, { ...(devices.get(id) || {}), online: true, lastSeen: Date.now() });
      pushTime(id);
      publishTo(id, 'state', buildStatePayload(), { qos: 1, retain: true });
      if (quotaPayload) publishTo(id, 'quota', quotaPayload, { qos: 1, retain: true });
      if (rankPayload) publishTo(id, 'rank', rankPayload, { qos: 1, retain: true });
      if (tokensPayload) publishTo(id, 'tokens', tokensPayload, { qos: 1, retain: true });
      if (choreoPayload && devices.get(id)?.prefs?.choreo !== false) {
        publishTo(id, 'choreo', choreoPayload, { qos: 1, retain: true });
      }
      const now = Date.now();
      if (now - lastSyncQuotaAt > 15000) {
        lastSyncQuotaAt = now;
        refreshQuota().catch(() => {});
      }
    }
  }

  function start() {
    client = mqttConnectFn(brokerUrl, {
      clientId: `nova-host-${process.pid}`,
      reconnectPeriod: 3000,
    });

    client.on('connect', () => {
      log(`connected to ${brokerUrl}`);
      client.subscribe(['nova/+/hello', 'nova/+/up'], { qos: 1 });
      refreshQuota().catch(() => {});
      refreshRank(false).catch(() => {});
      refreshChoreo('boot', 30000).catch(() => {});
      refreshTokens(false).catch(() => {});
    });

    client.on('message', (topic, buf) => {
      const parts = topic.split('/');
      if (parts.length !== 3 || parts[0] !== 'nova') return;
      const [, id, suffix] = parts;
      let data;
      try {
        data = JSON.parse(buf.toString('utf8'));
      } catch {
        return;
      }
      if (suffix === 'hello') handleHello(id, data);
      else if (suffix === 'up') handleUp(id, data);
    });

    quotaTimer = setInterval(() => refreshQuota().catch(() => {}), 5 * 60 * 1000);
    rankTimer = setInterval(() => refreshRank(true).catch(() => {}), 6 * 60 * 60 * 1000);
    hbTimer = setInterval(() => {
      for (const id of onlineIds()) pushTime(id);
    }, 30 * 1000);
    choreoTimer = setInterval(() => refreshChoreo('timer').catch(() => {}), aiCfg.llm.intervalMs);
    tokensTimer = setInterval(() => refreshTokens(false).catch(() => {}), 60 * 1000);
  }

  function onSessionEvent(session, event, tokenMeter) {
    const sid = String(session?.id || event?.sessionId || 'default');
    const cur = sessions.get(sid) || {
      running: false,
      waiting: false,
      error: false,
      lastTool: '',
      title: '',
      errMsg: '',
      msg: '',
      updatedAt: Date.now(),
    };
    const type = String(event?.type || '');
    const data = event?.data || {};

    if (data?.model) cur.model = String(data.model);

    switch (type) {
      case 'model/selection':
        if (data.model) cur.model = String(data.model);
        break;
      case 'session/title':
        // SessionTitleEventData { title, messageSeqs, source }；last-wins。
        cur.title = String(data.title || '').replace(/\s+/g, ' ').trim().slice(0, 20);
        break;
      case 'turn/start':
        cur.running = true;
        cur.waiting = false;
        cur.error = false;
        cur.lastTool = '';
        cur.errMsg = '';
        cur.msg = '思考中...';
        break;
      case 'tool/call': {
        cur.running = true;
        cur.error = false;
        const tool = String(data.name || data.toolName || 'tool');
        const arg = toolArgPreview(data.arguments).slice(0, 42);
        cur.lastTool = tool;
        cur.msg = arg ? `${tool}: ${arg}` : `${tool}`;
        break;
      }
      case 'tool/result':
        cur.running = true;
        if (data.error || data.message?.isError) {
          cur.msg = cur.lastTool ? `${cur.lastTool} 返回异常` : '工具执行异常';
        } else if (cur.lastTool) {
          cur.msg = `整理 ${cur.lastTool} 结果...`;
        }
        break;
      case 'approval/asked':
        cur.waiting = true;
        cur.msg = `等待审批: ${String(data.toolName || 'tool')}`;
        break;
      case 'approval/decided':
        cur.waiting = false;
        break;
      case 'turn/end': {
        const wasRunning = cur.running;
        cur.running = false;
        cur.waiting = false;
        cur.error = data.reason?.kind === 'error';
        if (cur.error) {
          // reason.error 是 LlmFailure { message, code, status?, requestId? }。
          // 以前整个丢掉，屏幕上永远只看到「本轮异常结束」四个字。
          const failure = data.reason?.error || {};
          const detail = String(failure.message || failure.reason || failure.code || '').trim();
          cur.errMsg = detail.slice(0, 120);
          cur.msg = cur.errMsg ? `本轮异常结束 · ${cur.errMsg}` : '本轮异常结束';
        } else {
          cur.errMsg = '';
          cur.msg = '待命中';
        }
        {
          const pct = readCtxPct(session, tokenMeter);
          if (pct != null) cur.ctxPct = pct;
        }
        if (wasRunning) {
          if (cur.error) {
            // 播具体原因，不是套话。dedupKey 用错误原文，连着同样的错只播一次。
            const detail = cur.errMsg ? clipForTts(cur.errMsg, 34) : '';
            const line = detail ? `出错了，${detail}` : '唔，刚刚好像出了一点小差错，快来看看呀！';
            const key = `err:${cur.errMsg || 'unknown'}`;
            if (!ttsDedupHit(key)) {
              publishTts(line, { force: true }).catch(() => {});
            }
          } else {
            // 结束那句以前没传 force，被 4 秒节流吃掉；传了之后要靠 task 维度去重。
            const key = `turnend:${sid}:${cur.updatedAt}`;
            if (!ttsDedupHit(key)) {
              publishTts(clipForTts('主人，刚刚的任务完成啦！'), { force: true }).catch(() => {});
            }
          }
          refreshChoreo('turn-end', 120000).catch(() => {});
        }
        break;
      }
      case 'step/end': {
        // DSH 没有 context/usage 事件；占用率从 tokenMeter 的投影现取。
        const pct = readCtxPct(session, tokenMeter);
        if (pct != null) cur.ctxPct = pct;
        break;
      }
      default:
        break;
    }

    cur.updatedAt = Date.now();
    sessions.set(sid, cur);
    flushState(false);
  }

  function onSessionDisposed(session) {
    const sid = String(session?.id || '');
    if (sid && sessions.delete(sid)) {
      flushState(true);
    }
  }

  async function onApprovalRequest(request, next) {
    const ids = onlineIds();
    if (deviceApproval === 'off' || ids.length === 0 || !client?.connected) {
      return next();
    }

    const apId = `ap_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`;
    const tool = String(request?.toolName || 'Tool');
    const reason = String(
      request?.displayReason?.zh || request?.displayReason?.en || request?.reason || '需要确认后继续',
    ).slice(0, 140);

    const payload = { t: 'approval', id: apId, tool, reason, ttl: fallbackMs };
    for (const id of ids) {
      publishTo(id, 'approval', payload, { qos: 1, retain: false });
    }
    // 播具体 reason，而不是固定套话。reason 原文有 140 字，必须压到 TTS 能扛的长度，
    // 否则设备侧整条静默丢弃。结构：<工具名> 需要确认：<reason 前 30 字>
    publishTts(clipForTts(`${tool} 需要确认：${reason}`, TTS_MAX_CHARS), { force: true }).catch(() => {});

    const waitDevice = new Promise((resolve) => {
      const timer = setTimeout(() => {
        settleApproval(apId, { kind: 'timeout' }, { notifyDeviceEnd: true });
      }, fallbackMs);
      pendingApprovals.set(apId, { resolve, deviceIds: ids, timer });
      request?.signal?.addEventListener?.(
        'abort',
        () => settleApproval(apId, { kind: 'abort' }, { notifyDeviceEnd: true }),
        { once: true },
      );
    });

    if (deviceApproval === 'mirror') {
      const local = Promise.resolve()
        .then(() => next())
        .then((outcome) => ({ kind: 'local', outcome }), () => ({ kind: 'local', outcome: 'unavailable' }));
      const winner = await Promise.race([local, waitDevice]);
      if (winner.kind === 'local') {
        settleApproval(apId, { kind: 'local' }, { notifyDeviceEnd: true });
        return winner.outcome;
      }
      if (winner.kind === 'decided') return winner.outcome;
      return local.then((r) => r.outcome);
    }

    const res = await waitDevice;
    if (res.kind === 'decided') return res.outcome;
    if (res.kind === 'abort') return 'rejected';
    return next();
  }

  function stop() {
    if (closed) return;
    closed = true;
    if (stateTimer) clearTimeout(stateTimer);
    if (quotaTimer) clearInterval(quotaTimer);
    if (rankTimer) clearInterval(rankTimer);
    if (hbTimer) clearInterval(hbTimer);
    if (choreoTimer) clearInterval(choreoTimer);
    if (tokensTimer) clearInterval(tokensTimer);
    for (const apId of [...pendingApprovals.keys()]) {
      settleApproval(apId, { kind: 'abort' }, { notifyDeviceEnd: true });
    }
    client?.end?.(true);
  }

  return {
    start,
    stop,
    onSessionEvent,
    onSessionDisposed,
    onApprovalRequest,
    buildStatePayload,
    refreshQuota,
    refreshRank,
    refreshChoreo,
    refreshTokens,
    publishTts,
    publishTo,
    onlineIds,
    pushTime,
  };
}
