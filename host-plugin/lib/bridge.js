import mqtt from 'mqtt';
import { fetchAllQuota, fetchModelDialRank } from './fetchers.js';
import { generateChoreography, loadAiConfig, synthesizeSpeechAdpcm } from './ai.js';

const TTS_CHUNK_BYTES = 1800;

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

  const devices = new Map(); // id -> { online, lastSeen, meta, prefs, stat }
  const sessions = new Map(); // sessionId -> { running, waiting, error, lastTool, msg, ctxPct, model, updatedAt }
  const pendingApprovals = new Map(); // apId -> { resolve, deviceIds, timer }

  let client = null;
  let quotaPayload = null;
  let rankPayload = null;
  let choreoPayload = null;
  let recentAnims = [];
  let recentMood = '';
  let lastStateJson = '';
  let lastStateSentAt = 0;
  let lastChoreoAt = 0;
  let lastTtsAt = 0;
  let lastSyncQuotaAt = 0;
  let ttsSeqId = 1;
  let stateTimer = null;
  let quotaTimer = null;
  let rankTimer = null;
  let hbTimer = null;
  let choreoTimer = null;
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
    if (run === 0 && chosen?.error) payload.err = true;
    if (ctxPct != null) payload.ctx = { pct: Math.max(0, Math.min(100, Math.round(ctxPct))) };
    if (model) payload.model = model;
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
  }

  function onSessionEvent(session, event) {
    const sid = String(session?.id || event?.sessionId || 'default');
    const cur = sessions.get(sid) || {
      running: false,
      waiting: false,
      error: false,
      lastTool: '',
      msg: '',
      updatedAt: Date.now(),
    };
    const type = String(event?.type || '');
    const data = event?.data || {};

    if (session?.model) cur.model = String(session.model);
    if (data?.model) cur.model = String(data.model);

    switch (type) {
      case 'turn/start':
        cur.running = true;
        cur.waiting = false;
        cur.error = false;
        cur.lastTool = '';
        cur.msg = '思考中...';
        break;
      case 'tool/call': {
        cur.running = true;
        cur.error = false;
        const tool = String(data.toolName || data.name || 'tool');
        const arg = String(data.command || data.path || data.query || '').slice(0, 42);
        cur.lastTool = tool;
        cur.msg = arg ? `${tool}: ${arg}` : `${tool}`;
        break;
      }
      case 'tool/result':
        cur.running = true;
        if (data.isError || data.error) {
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
        cur.error = data.reason === 'error';
        cur.msg = cur.error ? '本轮异常结束' : '待命中';
        if (wasRunning) {
          const line = cur.error
            ? '唔，刚刚好像出了一点小差错，快来看看呀！'
            : '主人，刚刚的任务完成啦！';
          publishTts(line).catch(() => {});
          refreshChoreo('turn-end', 120000).catch(() => {});
        }
        break;
      }
      case 'context/usage':
        if (data.percent != null) cur.ctxPct = Number(data.percent);
        break;
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
    publishTts(`主人，${tool} 工具请求确认，快来看看吧！`, { force: true }).catch(() => {});

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
    publishTts,
    publishTo,
    onlineIds,
    pushTime,
  };
}
