import mqtt from 'mqtt';
import { basename } from 'node:path';
import { fetchAllQuota, fetchModelDialRank } from './fetchers.js';
import {
  clipByDisplayWidth,
  DEVICE_REPLY_WIDTH,
  generateChatReply,
  generateChoreography,
  loadAiConfig,
  stripMarkdownToPlain,
  summarizeSessionReply,
  synthesizeSpeechAdpcm,
} from './ai.js';
import { createMicReassembler, loadMiMoApiKey, transcribeMiMoWav } from './asr.js';
import { createTokenCollector } from './tokens.js';
import { createPowerStore, stageNameOf } from './power.js';

const TTS_CHUNK_BYTES = 1800;

const ANSI_RE = /\x1b\[[0-9;?]*[A-Za-z]/g;

/** 一行化：去 ANSI 转义、控制字符，压空白。设备上只有一行位置，换行会变成空白。 */
export function sanitizeOneLine(raw, limit = 200) {
  return String(raw ?? '')
    .replace(ANSI_RE, '')
    .replace(/[\u0000-\u001f\u007f]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, limit);
}

/** 路径折叠到末两段：/Volumes/…/src/ui/page_home.cpp → ui/page_home.cpp */
export function foldPath(raw) {
  const s = sanitizeOneLine(raw, 200).replace(/[\\/]+$/, '');
  const parts = s.split(/[\\/]/).filter(Boolean);
  if (parts.length <= 2) return s;
  return parts.slice(-2).join('/');
}

/**
 * bash 命令压成人类能扫一眼的短句：
 * cd /tmp && node -e '...'  →  node -e …
 * 只做确定性清洗（不猜语义），所以同一命令永远得到同一结果。
 */
export function shortenCommand(raw, limit = 34) {
  let s = sanitizeOneLine(raw, 400);
  if (!s) return '';
  s = s.replace(/^(?:\/(?:usr\/)?bin\/)?(?:bash|sh|zsh)\s+-[a-z]*c\s+/i, '');
  for (let i = 0; i < 3; i++) {
    const next = s.replace(/^cd\s+(?:'[^']*'|"[^"]*"|[^\s&;]+)\s*(?:&&|;)\s*/i, '');
    if (next === s) break;
    s = next;
  }
  for (let i = 0; i < 3; i++) {
    const next = s.replace(/^[A-Za-z_][A-Za-z0-9_]*=(?:'[^']*'|"[^"]*"|\S+)\s+/, '');
    if (next === s) break;
    s = next;
  }
  s = s.replace(/^(?:bash|sh|zsh)[:：]\s*/i, '');
  // bash -c "整段"：先把外层引号剥掉，再判断是不是内联脚本
  s = s.replace(/^"([^"]*)"$/, '$1').replace(/^'([^']*)'$/, '$1');
  // 内联脚本（node -e '...' / python -c "..."）：在引号前收住，别把整段脚本搬上屏
  const q = s.search(/['"]/);
  if (q > 0 && s.length - q > 24) s = `${s.slice(0, q).trim()} …`;
  s = s.replace(/\s+/g, ' ').trim();
  return s.length <= limit ? s : `${s.slice(0, limit - 1)}…`;
}

/** 会话标题：剥 @ 前缀、路径取末段、超长中间省略（保留头尾更可读）。 */
export function foldTitle(raw) {
  let s = sanitizeOneLine(raw, 80).replace(/^@+/, '');
  if (!s) return '';
  if (/^(?:\/|~\/)/.test(s)) {
    const parts = s.split(/[\\/]/).filter(Boolean);
    s = parts[parts.length - 1] || s;
  }
  if (s.length > 20) s = `${s.slice(0, 9)}…${s.slice(-8)}`;
  return s;
}

/** 从 tool/call 的 arguments 里摘一段人类可读预览（arguments 是 JSON 字符串）。 */
function toolArgPreview(raw) {
  if (raw == null) return '';
  if (typeof raw === 'string') {
    try {
      return toolArgPreview(JSON.parse(raw));
    } catch {
      return shortenCommand(raw);
    }
  }
  if (typeof raw !== 'object') return sanitizeOneLine(raw, 34);
  for (const key of ['command', 'path', 'file_path', 'query', 'pattern', 'url', 'description', 'prompt']) {
    const v = raw[key];
    if (typeof v !== 'string' || !v.trim()) continue;
    if (key === 'command') return shortenCommand(v);
    if (key === 'path' || key === 'file_path') return foldPath(v);
    return sanitizeOneLine(v, 34);
  }
  const firstString = Object.values(raw).find((v) => typeof v === 'string' && v.trim());
  return typeof firstString === 'string' ? sanitizeOneLine(firstString, 34) : '';
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
const TTS_MAX_CHARS = DEVICE_REPLY_WIDTH;
function clipForTts(text, limit = TTS_MAX_CHARS) {
  return clipByDisplayWidth(stripMarkdownToPlain(text), limit);
}

function cleanBoardName(raw, fallback = '') {
  let s = sanitizeOneLine(raw, 80).replace(/^@+/, '');
  if (!s) return fallback;
  if (/^(?:\/|~\/)/.test(s)) {
    const base = basename(s.replace(/[\\/]+$/, ''));
    if (base) s = base;
  }
  return s.length > 28 ? `${s.slice(0, 27)}…` : s;
}

function cwdBasename(raw) {
  const s = sanitizeOneLine(raw, 200).replace(/[\\/]+$/, '');
  if (!s) return '';
  return basename(s) || '';
}

function extractAssistantText(data) {
  const content = data?.message?.content ?? data?.content;
  if (typeof content === 'string') return content.trim();
  if (Array.isArray(content)) {
    return content
      .filter((part) => part && (part.type === 'text' || typeof part.text === 'string'))
      .map((part) => String(part.text || ''))
      .join('\n')
      .trim();
  }
  return '';
}

function fmtStepClock(msAgo) {
  const totalSec = Math.max(0, Math.floor(msAgo / 1000));
  if (totalSec < 60) return `${totalSec}s`;
  const m = Math.floor(totalSec / 60);
  const s = totalSec % 60;
  return `${m}:${String(s).padStart(2, '0')}`;
}

export function createNovaBridge(options = {}) {
  const brokerUrl = options.brokerUrl || process.env.NOVA_MQTT_URL || 'mqtt://192.168.31.15:1883';
  const fallbackMs = Number(options.fallbackMs ?? 20000);
  const deviceApproval = options.deviceApproval || 'mirror'; // 'prefer' | 'mirror' | 'off'
  const log = options.log || (() => {});
  const warn = (msg) => (typeof log.warn === 'function' ? log.warn(msg) : log(msg));
  const aiCfg = loadAiConfig(options);
  const ttsSynth = options.synthesizeSpeech || synthesizeSpeechAdpcm;
  const transcribeAudio = options.transcribeAudio || ((wav) => transcribeMiMoWav(wav, loadMiMoApiKey(aiCfg)));
  const chatReplyGenerator = options.chatReplyGenerator || generateChatReply;
  const replySummarizer = options.replySummarizer || summarizeSessionReply;
  const hasCustomChatReply = typeof options.chatReplyGenerator === 'function';
  let sessionController = options.sessionController || null;
  // 拿实时标题：list() 的 SessionSummary 不含 title，重启后收不到历史 session/title 事件，
  // 只能靠 sessionTitle.get(session) 补，否则看板会退化成 cwd 末段（工作区名）。
  let titleSource = options.titleSource || null;
  const pendingChatQueue = []; // { id, clipId, text, target, mode, enqueuedAt }
  const activeFollowAborts = new Set();
  let monoReqSeq = 1;
  let lastSayText = '';
  let lastSayAt = 0;
  const micReassembler = createMicReassembler(options.micReassemblerOptions);
  const quotaFetcher = options.fetchAllQuota || fetchAllQuota;
  const choreoGen = options.choreoGenerator || generateChoreography;
  const choreoFreshMs = Number(options.choreoFreshMs ?? 15 * 60 * 1000);
  const rankFetcher = options.fetchModelDialRank || fetchModelDialRank;
  const mqttConnectFn = options.mqttConnect || ((url, opts) => mqtt.connect(url, opts));
  // token 页的数据源是会话日志而不是 ctx 服务：`ctx.sessions` 只列 live session，
  // 拿不到历史，而累计页要的正是历史。见 lib/tokens.js 顶部注释。
  const tokenCollector = createTokenCollector({
    log,
    enabled: options.tokensEnabled !== false,
    dshHome: options.dshHome,
  });
  const powerStore = createPowerStore({
    log,
    enabled: options.powerLogEnabled !== false,
    dshHome: options.dshHome,
    ...(options.powerStoreOptions || {}),
  });

  const devices = new Map(); // id -> { online, lastSeen, meta, prefs, stat }
  const sessions = new Map(); // sessionId -> { running, waiting, error, lastTool, msg, ctxPct, model, updatedAt }
  const pendingApprovals = new Map(); // apId -> { resolve, deviceIds, timer }
  const chatHistories = new Map(); // deviceId -> Array<{ role: 'user' | 'assistant', content: string }>

  let client = null;
  let quotaPayload = null;
  let rankPayload = null;
  let choreoPayload = null;
  let tokensPayload = null;
  let recentAnims = [];
  let recentMood = '';
  let lastStateJson = '';
  let lastStateSentAt = 0;
  let lastSessionsJson = '';
  let lastSessionsSentAt = 0;
  let sessionsTimer = null;
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

  function ttsTargetIds(filterIds = null) {
    const allow = Array.isArray(filterIds) ? new Set(filterIds) : null;
    const out = [];
    for (const [id, info] of devices.entries()) {
      if (!info.online) continue;
      if (allow && !allow.has(id)) continue;
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
    client.publish(`nova/${id}/${suffix}`, JSON.stringify(obj), { qos, retain }, (err) => {
      if (err) warn(`nova-buddy: publish 失败 nova/${id}/${suffix}: ${err.message || err}`);
    });
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

  function buildSessionsPayload() {
    const now = Date.now();
    const list = [];
    for (const [sid, s] of sessions.entries()) {
      if (s.origin === 'subagent' || s.archived) continue;
      const prio = s.waiting ? 3 : (s.running ? 2 : (s.error ? 1 : 0));
      list.push({ sid, s, prio });
    }
    list.sort((a, b) => (b.prio - a.prio) || ((b.s.updatedAt || 0) - (a.s.updatedAt || 0)));

    const items = list.slice(0, 4).map(({ sid, s }) => {
      const st = s.waiting ? 'wait' : (s.running ? 'run' : (s.error ? 'err' : 'idle'));
      const el = (s.running && s.startedAt) ? Math.max(0, Math.floor((now - s.startedAt) / 1000)) : 0;
      const name = cleanBoardName(s.boardName || s.title || s.cwd || sid, sid.slice(0, 14));
      const cwd = s.cwd || '';
      let act = s.lastAction || '';
      if (!act) {
        act = s.waiting ? `等待审批: ${s.lastTool || 'Tool'}` : (s.running ? '处理中' : '已停下');
      }
      let note = '';
      if (s.waiting && s.waitReason) {
        note = sanitizeOneLine(s.waitReason, 56);
      } else if (s.error && s.errMsg) {
        note = sanitizeOneLine(s.errMsg, 56);
      } else if (!s.running && s.updatedAt) {
        const mins = Math.max(0, Math.floor((now - s.updatedAt) / 60000));
        note = mins > 0 ? `${mins} 分钟前停下` : '刚刚停下';
      } else if (s.lastReplySnippet) {
        note = s.lastReplySnippet;
      }

      const steps = Array.isArray(s.steps)
        ? s.steps.slice(0, 3).map((sp) => ({
            t: fmtStepClock(now - (sp.at || now)),
            s: sp.tool || 'tool',
            a: sp.arg || '',
          }))
        : [];

      return { id: sid, name, cwd, st, el, act, note, steps };
    });

    const payload = { t: 'sessions', items };
    if (lastSayText) {
      payload.lastSay = lastSayText;
      payload.lastAgo = lastSayAt > 0 ? Math.max(0, Math.floor((now - lastSayAt) / 1000)) : 0;
    }
    return payload;
  }

  function flushSessions(force = false) {
    const payload = buildSessionsPayload();
    const raw = JSON.stringify(payload);
    const now = Date.now();
    if (!force && raw === lastSessionsJson && now - lastSessionsSentAt < 5000) return;
    if (!force && now - lastSessionsSentAt < 400) {
      if (!sessionsTimer) {
        sessionsTimer = setTimeout(() => {
          sessionsTimer = null;
          flushSessions(true);
        }, 400);
      }
      return;
    }
    lastSessionsJson = raw;
    lastSessionsSentAt = now;
    publishAll('sessions', payload, { qos: 1, retain: true });
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
    if (!aiCfg.llm?.primary?.apiKey && !aiCfg.llm?.fallback?.apiKey && !options.choreoGenerator) {
      return null;
    }
    lastChoreoAt = now;
    const c = await choreoGen(buildContextSummary(reason), aiCfg, log, { recentAnims, recentMood });
    if (c) {
      // retain:false —— retained 剧本会在设备冷启动/重连时被 broker 重放，
      // 半夜播「主人晚上好呀」。设备重连由 hello 补推新鲜的（见 handleHello）。
      choreoPayload = { ...c, at: Date.now() };
      recentMood = c.mood || '';
      recentAnims = c.steps.map((s) => s.anim);
      publishAll('choreo', choreoPayload, { qos: 1, retain: false });
    }
    return c;
  }

  async function publishTts(text, opts = {}) {
    if (!String(text || '').trim()) return false;
    const now = Date.now();
    if (!opts.force && now - lastTtsAt < 4000) return false;
    const targets = ttsTargetIds(opts.targetIds);
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

  function choreoStillFresh() {
    return Boolean(choreoPayload && Date.now() - (choreoPayload.at || 0) < choreoFreshMs);
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
      publishTo(id, 'sessions', buildSessionsPayload(), { qos: 1, retain: true });
      if (quotaPayload) publishTo(id, 'quota', quotaPayload, { qos: 1, retain: true });
      if (rankPayload) publishTo(id, 'rank', rankPayload, { qos: 1, retain: true });
      if (tokensPayload) publishTo(id, 'tokens', tokensPayload, { qos: 1, retain: true });
      if (choreoStillFresh() && prefs?.choreo !== false) {
        publishTo(id, 'choreo', choreoPayload, { qos: 1, retain: false });
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
      powerStore.recordStat(id, data);
    } else if (data?.evt === 'pwr') {
      devices.set(id, { ...(devices.get(id) || {}), online: true, lastSeen: Date.now(), pwr: data });
      powerStore.recordPowerEvent(id, data);
      log(
        `device ${id} pwr ${data.why}: ${data.pct}% (${data.mv}mV ${stageNameOf(data.st)} vb=${data.vb ?? 0} vs=${data.vs ?? 0} mohm=${data.mohm ?? '-'})`,
      );
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
      publishTo(id, 'sessions', buildSessionsPayload(), { qos: 1, retain: true });
      if (quotaPayload) publishTo(id, 'quota', quotaPayload, { qos: 1, retain: true });
      if (rankPayload) publishTo(id, 'rank', rankPayload, { qos: 1, retain: true });
      if (tokensPayload) publishTo(id, 'tokens', tokensPayload, { qos: 1, retain: true });
      if (choreoStillFresh() && devices.get(id)?.prefs?.choreo !== false) {
        publishTo(id, 'choreo', choreoPayload, { qos: 1, retain: false });
      }
      const now = Date.now();
      if (now - lastSyncQuotaAt > 15000) {
        lastSyncQuotaAt = now;
        refreshQuota().catch(() => {});
      }
    } else if (data?.evt === 'chat_send') {
      devices.set(id, { ...(devices.get(id) || {}), online: true, lastSeen: Date.now() });
      const clipId = Number(data.id) || 0;
      const text = String(data.text || '').trim();
      const target = typeof data.target === 'string' ? data.target.trim() : '';
      const mode = data.mode === 'steer' ? 'steer' : 'queue';
      if (!text) return;
      handleChatSend(id, clipId, text, target, mode).catch((err) => {
        warn(`nova-buddy: chat_send 异常 ${err?.message || err}`);
      });
    } else if (data?.evt === 'chat_tts') {
      devices.set(id, { ...(devices.get(id) || {}), online: true, lastSeen: Date.now() });
      const text = String(data.text || '').trim();
      if (!text) return;
      publishTts(clipForTts(text), { force: true, targetIds: [id] }).catch((err) =>
        warn(`nova-buddy: chat_tts 播报失败 ${err?.message || err}`),
      );
    }
  }

  async function syncSessionsFromController() {
    if (!sessionController || typeof sessionController.list !== 'function') return;
    try {
      const res = await sessionController.list({}, AbortSignal.timeout(3000));
      const items = Array.isArray(res?.items) ? res.items : [];
      const seen = new Set();
      for (const item of items) {
        const sid = String(item?.sessionId || '');
        if (!sid || item?.origin === 'subagent' || item?.archived) continue;
        seen.add(sid);
        const cur = sessions.get(sid) || {
          running: false,
          waiting: false,
          error: false,
          lastTool: '',
          title: '',
          boardName: '',
          cwd: '',
          origin: item?.origin || '',
          errMsg: '',
          msg: '',
          lastAction: '',
          steps: [],
          updatedAt: Date.now(),
        };
        cur.running = Boolean(item.running);
        if (item.cwd) cur.cwd = cwdBasename(item.cwd);
        const liveTitle = titleSource ? titleSource(sid) : '';
        if (liveTitle) {
          cur.boardName = cleanBoardName(liveTitle, cur.boardName);
          cur.title = foldTitle(liveTitle);
        } else if (item.title) {
          cur.boardName = cleanBoardName(item.title, cur.boardName);
          if (!cur.title) cur.title = foldTitle(item.title);
        }
        if (item.origin) cur.origin = item.origin;
        if (cur.running && !cur.startedAt) cur.startedAt = Date.now();
        if (typeof item.updatedAt === 'number') cur.updatedAt = item.updatedAt;
        sessions.set(sid, cur);
      }
      if (seen.size > 0) {
        for (const sid of [...sessions.keys()]) {
          const cur = sessions.get(sid);
          if (!seen.has(sid) && !cur?.running && !cur?.waiting) {
            sessions.delete(sid);
          }
        }
      }
      flushState(false);
      flushSessions(false);
    } catch (err) {
      warn(`nova-buddy: sessionController.list 失败 ${err?.message || err}`);
    }
  }

  function setTitleSource(fn) {
    titleSource = typeof fn === 'function' ? fn : null;
    if (titleSource) syncSessionsFromController().catch(() => {});
  }

  function setSessionController(ctrl) {
    sessionController = ctrl || null;
    if (!sessionController) return;
    syncSessionsFromController().catch(() => {});
    while (pendingChatQueue.length > 0) {
      const item = pendingChatQueue.shift();
      if (Date.now() - item.enqueuedAt > 15000) {
        publishTo(item.id, 'chat', {
          t: 'delivery',
          id: item.clipId,
          kind: 'fail',
          target: item.target,
          text: '宿主会话服务启动超时',
        }, { qos: 1, retain: false });
        continue;
      }
      handleChatSend(item.id, item.clipId, item.text, item.target, item.mode).catch((err) => {
        warn(`nova-buddy: queued chat_send 异常 ${err?.message || err}`);
      });
    }
  }

  async function resolveTargetSession(requestedTarget) {
    if (!requestedTarget) {
      return { ok: false, err: '未选择目标会话' };
    }
    const known = sessions.get(requestedTarget);
    if (known?.waiting) {
      return { ok: false, err: '目标会话正在等审批，请先处理审批' };
    }
    if (typeof sessionController?.list === 'function') {
      try {
        const res = await sessionController.list({}, AbortSignal.timeout(3000));
        const items = Array.isArray(res?.items) ? res.items : [];
        const match = items.find((it) => it?.sessionId === requestedTarget);
        if (!match || match.archived || match.origin === 'subagent') {
          return { ok: false, err: '目标会话已换段或已关闭，请重选' };
        }
        return {
          ok: true,
          sessionId: match.sessionId,
          name: cleanBoardName(match.title || known?.boardName || known?.title || cwdBasename(match.cwd), requestedTarget.slice(0, 14)),
        };
      } catch (err) {
        return { ok: false, err: `会话校验失败: ${sanitizeOneLine(err?.message || 'unknown', 24)}` };
      }
    }
    if (!known) {
      return { ok: false, err: '目标会话已失效，请重选' };
    }
    return {
      ok: true,
      sessionId: requestedTarget,
      name: cleanBoardName(known.boardName || known.title || known.cwd, requestedTarget.slice(0, 14)),
    };
  }

  async function handleLegacyChatSend(id, clipId, text) {
    const prevHistory = chatHistories.get(id) || [];
    try {
      const rawReply = await chatReplyGenerator(
        text,
        prevHistory.slice(-12),
        buildContextSummary('chat'),
        aiCfg,
        log,
      );
      const reply = clipByDisplayWidth(stripMarkdownToPlain(rawReply), DEVICE_REPLY_WIDTH);
      if (!reply) throw new Error('empty reply');
      const nextHistory = [
        ...prevHistory,
        { role: 'user', content: text },
        { role: 'assistant', content: reply },
      ].slice(-12);
      chatHistories.set(id, nextHistory);
      lastSayText = reply;
      lastSayAt = Date.now();
      publishTo(id, 'chat', { t: 'reply', id: clipId, ok: true, text: reply }, { qos: 1, retain: false });
      await publishTts(clipForTts(reply), { force: true, targetIds: [id] });
    } catch (err) {
      log(`chat_send error for ${id}: ${err?.message || err}`);
      publishTo(id, 'chat', { t: 'reply', id: clipId, ok: false, err: '回复超时' }, { qos: 1, retain: false });
    }
  }

  async function handleChatSend(id, clipId, text, target = '', mode = 'queue') {
    if (!sessionController) {
      if (hasCustomChatReply && !target) {
        return handleLegacyChatSend(id, clipId, text);
      }
      if (pendingChatQueue.length >= 8) pendingChatQueue.shift();
      pendingChatQueue.push({ id, clipId, text, target, mode, enqueuedAt: Date.now() });
      publishTo(id, 'chat', {
        t: 'delivery',
        id: clipId,
        kind: 'sent',
        pos: 1,
        target,
        name: cleanBoardName(sessions.get(target)?.boardName || sessions.get(target)?.title || '', ''),
      }, { qos: 1, retain: false });
      return;
    }

    const resolved = await resolveTargetSession(target);
    if (!resolved.ok) {
      publishTo(id, 'chat', {
        t: 'delivery',
        id: clipId,
        kind: 'fail',
        target,
        text: resolved.err,
      }, { qos: 1, retain: false });
      return;
    }

    const { sessionId, name } = resolved;
    const safeMode = mode === 'steer' ? 'steer' : 'queue';
    const requestId = `${id}:${clipId}:${monoReqSeq++}`;
    const ac = new AbortController();
    activeFollowAborts.add(ac);
    const timeout = setTimeout(() => ac.abort(), 10 * 60 * 1000);

    try {
      const frames = sessionController.follow({ address: { kind: 'session', sessionId } }, ac.signal);
      await sessionController.prompt({
        requestId,
        sessionId,
        mode: safeMode,
        content: [{ type: 'text', text }],
      }, ac.signal);

      publishTo(id, 'chat', {
        t: 'delivery',
        id: clipId,
        kind: 'sent',
        pos: safeMode === 'steer' ? 0 : 1,
        target: sessionId,
        name,
      }, { qos: 1, retain: false });

      let admitted = false;
      const turnTexts = [];
      for await (const frame of frames) {
        if (closed) break;
        if (frame?.type !== 'event' || !frame.event) continue;
        const ev = frame.event;
        if (ev.type === 'user/message') {
          const src = ev.data?.source;
          if (src?.rpcId === requestId && (!src.kind || src.kind === 'user')) {
            admitted = true;
            publishTo(id, 'chat', {
              t: 'delivery',
              id: clipId,
              kind: 'run',
              secs: 0,
              target: sessionId,
              name,
            }, { qos: 1, retain: false });
          }
          continue;
        }
        if (!admitted) continue;
        if (ev.type === 'assistant/message') {
          const chunk = extractAssistantText(ev.data);
          if (chunk) turnTexts.push(chunk);
          continue;
        }
        if (ev.type === 'turn/end') {
          const kind = ev.data?.reason?.kind;
          if (kind === 'error') {
            const errMsg = ev.data?.reason?.error?.message || '本轮异常结束';
            const outErr = await replySummarizer({ userText: text, errText: errMsg }, aiCfg, log);
            publishTo(id, 'chat', {
              t: 'delivery',
              id: clipId,
              kind: 'fail',
              target: sessionId,
              name,
              text: outErr,
            }, { qos: 1, retain: false });
            await publishTts(clipForTts(outErr), { force: true, targetIds: [id] });
          } else if (turnTexts.length === 0) {
            publishTo(id, 'chat', {
              t: 'delivery',
              id: clipId,
              kind: 'mute',
              target: sessionId,
              name,
            }, { qos: 1, retain: false });
          } else {
            const summary = await replySummarizer({
              userText: text,
              assistantText: turnTexts.join('\n'),
            }, aiCfg, log);
            if (!summary) {
              publishTo(id, 'chat', {
                t: 'delivery',
                id: clipId,
                kind: 'mute',
                target: sessionId,
                name,
              }, { qos: 1, retain: false });
            } else {
              lastSayText = summary;
              lastSayAt = Date.now();
              publishTo(id, 'chat', {
                t: 'delivery',
                id: clipId,
                kind: 'reply',
                target: sessionId,
                name,
                text: summary,
                ago: 0,
              }, { qos: 1, retain: false });
              await publishTts(clipForTts(summary), { force: true, targetIds: [id] });
              flushSessions(true);
            }
          }
          break;
        }
      }
    } catch (err) {
      if (!closed && err?.name !== 'AbortError') {
        const msg = clipByDisplayWidth(`未送达：${sanitizeOneLine(err?.message || '投递失败', 28)}`, DEVICE_REPLY_WIDTH);
        publishTo(id, 'chat', {
          t: 'delivery',
          id: clipId,
          kind: 'fail',
          target: sessionId,
          name,
          text: msg,
        }, { qos: 1, retain: false });
      }
    } finally {
      clearTimeout(timeout);
      activeFollowAborts.delete(ac);
      ac.abort();
    }
  }

  function handleMicMessage(id, sub, buf) {
    if (sub === 'chunk') {
      micReassembler.chunk(id, buf);
      return;
    }
    if (sub !== 'start' && sub !== 'end') return;
    let data;
    try {
      data = JSON.parse(buf.toString('utf8'));
    } catch {
      warn(`nova-buddy: mic/${sub} 载荷不是合法 JSON`);
      return;
    }
    if (sub === 'start') {
      micReassembler.start(id, data);
      return;
    }
    const res = micReassembler.end(id, data);
    if (!res) return;
    if (!res.ok) {
      publishTo(id, 'mic_ack', { id: res.id, ok: false, missing: res.missing }, { qos: 1, retain: false });
      return;
    }
    publishTo(id, 'mic_ack', { id: res.id, ok: true, file: 'mimo_asr', missing: 0 }, { qos: 1, retain: false });
    const startedAt = Date.now();
    Promise.resolve()
      .then(() => transcribeAudio(res.wav))
      .then((asr) => {
        const ms = Math.max(0, Math.round(asr?.ms ?? (Date.now() - startedAt)));
        const text = String(asr?.text || '').trim();
        if (asr?.ok && text) {
          publishTo(id, 'chat', { t: 'asr', id: res.id, ok: true, text, ms }, { qos: 1, retain: false });
        } else {
          const err = sanitizeOneLine(asr?.err || '未识别到语音', 60) || '未识别到语音';
          publishTo(id, 'chat', { t: 'asr', id: res.id, ok: false, err }, { qos: 1, retain: false });
        }
      })
      .catch((err) => {
        const msg = sanitizeOneLine(err?.message || '识别失败', 60) || '识别失败';
        publishTo(id, 'chat', { t: 'asr', id: res.id, ok: false, err: msg }, { qos: 1, retain: false });
      });
  }

  function start() {
    client = mqttConnectFn(brokerUrl, {
      clientId: `nova-host-${process.pid}`,
      reconnectPeriod: 3000,
    });

    client.on('connect', () => {
      log(`connected to ${brokerUrl}`);
      client.subscribe(['nova/+/hello', 'nova/+/up', 'nova/+/mic/+'], { qos: 1 });
      refreshQuota().catch(() => {});
      refreshRank(false).catch(() => {});
      refreshChoreo('boot', 30000).catch(() => {});
      refreshTokens(false).catch(() => {});
    });

    client.on('error', (e) => warn(`nova-buddy: mqtt error ${e?.message || e}`));
    client.on('offline', () => warn('nova-buddy: mqtt offline'));
    client.on('reconnect', () => log('nova-buddy: mqtt reconnecting'));

    client.on('message', (topic, buf) => {
      const parts = topic.split('/');
      if (parts[0] !== 'nova') return;
      if (parts.length === 4 && parts[2] === 'mic') {
        handleMicMessage(parts[1], parts[3], buf);
        return;
      }
      if (parts.length !== 3) return;
      const [, id, suffix] = parts;
      let data;
      try {
        data = JSON.parse(buf.toString('utf8'));
      } catch {
        warn(`nova-buddy: 上行载荷不是合法 JSON: ${suffix}`);
        return;
      }
      if (suffix === 'hello') handleHello(id, data);
      else if (suffix === 'up') handleUp(id, data);
    });

    quotaTimer = setInterval(
      () => refreshQuota().catch((e) => warn(`nova-buddy: 额度抓取失败 ${e?.message || e}`)),
      5 * 60 * 1000,
    );
    rankTimer = setInterval(
      () => refreshRank(true).catch((e) => warn(`nova-buddy: 榜单抓取失败 ${e?.message || e}`)),
      6 * 60 * 60 * 1000,
    );
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
      boardName: '',
      cwd: '',
      origin: '',
      errMsg: '',
      msg: '',
      lastAction: '',
      waitReason: '',
      lastReplySnippet: '',
      steps: [],
      startedAt: 0,
      updatedAt: Date.now(),
    };
    const type = String(event?.type || '');
    const data = event?.data || {};
    if (session?.header?.cwd && !cur.cwd) {
      cur.cwd = cwdBasename(session.header.cwd);
    }
    if (session?.origin && !cur.origin) {
      cur.origin = String(session.origin);
    }

    if (data?.model) cur.model = String(data.model);

    switch (type) {
      case 'model/selection':
        if (data.model) cur.model = String(data.model);
        break;
      case 'session/title':
        // SessionTitleEventData { title, messageSeqs, source }；last-wins。
        cur.title = foldTitle(data.title);
        cur.boardName = cleanBoardName(data.title, cur.boardName);
        break;
      case 'turn/start':
        cur.running = true;
        cur.waiting = false;
        cur.error = false;
        cur.waitReason = '';
        cur.lastTool = '';
        cur.errMsg = '';
        cur.msg = '思考中...';
        cur.startedAt = Date.now();
        break;
      case 'tool/call': {
        cur.running = true;
        cur.error = false;
        const tool = String(data.name || data.toolName || 'tool');
        const arg = toolArgPreview(data.arguments).slice(0, 34);
        cur.lastTool = tool;
        cur.msg = arg ? `${tool}: ${arg}` : `${tool}`;
        cur.lastAction = cur.msg;
        cur.steps = [{ at: Date.now(), tool, arg }, ...(cur.steps || [])].slice(0, 3);
        break;
      }
      case 'tool/result':
        cur.running = true;
        if (data.error || data.message?.isError) {
          cur.msg = cur.lastTool ? `${cur.lastTool} 返回异常` : '工具执行异常';
          cur.lastAction = cur.msg;
        } else if (cur.lastTool) {
          cur.msg = `整理 ${cur.lastTool} 结果...`;
        }
        break;
      case 'assistant/message': {
        const txt = stripMarkdownToPlain(extractAssistantText(data));
        if (txt) {
          cur.lastReplySnippet = txt.length > 42 ? `${txt.slice(0, 41)}…` : txt;
        }
        break;
      }
      case 'approval/asked':
        cur.waiting = true;
        cur.msg = `等待审批: ${String(data.toolName || 'tool')}`;
        cur.waitReason = sanitizeOneLine(data.reason || data.displayReason?.zh || '', 56);
        cur.lastAction = `${String(data.toolName || 'tool')}${cur.waitReason ? ` · ${cur.waitReason}` : ''}`;
        break;
      case 'approval/decided':
        cur.waiting = false;
        cur.waitReason = '';
        break;
      case 'turn/end': {
        const wasRunning = cur.running;
        cur.running = false;
        cur.waiting = false;
        cur.waitReason = '';
        cur.error = data.reason?.kind === 'error';
        if (cur.error) {
          // reason.error 是 LlmFailure { message, code, status?, requestId? }。
          // 以前整个丢掉，屏幕上永远只看到「本轮异常结束」四个字。
          const failure = data.reason?.error || {};
          const detail = String(failure.message || failure.reason || failure.code || '').trim();
          cur.errMsg = detail.slice(0, 120);
          cur.msg = cur.errMsg ? `本轮异常结束 · ${cur.errMsg}` : '本轮异常结束';
          cur.lastAction = cur.errMsg || '本轮异常结束';
        } else {
          cur.errMsg = '';
          cur.msg = '待命中';
          if (!cur.lastAction) cur.lastAction = '已停下';
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
              publishTts(line, { force: true }).catch((e) =>
                warn(`nova-buddy: TTS 播报失败 ${e?.message || e}`));
            }
          } else {
            // 结束那句以前没传 force，被 4 秒节流吃掉；传了之后要靠 task 维度去重。
            const key = `turnend:${sid}:${cur.updatedAt}`;
            if (!ttsDedupHit(key)) {
              publishTts(clipForTts('主人，刚刚的任务完成啦！'), { force: true }).catch((e) =>
                warn(`nova-buddy: TTS 播报失败 ${e?.message || e}`));
            }
          }
          refreshChoreo('turn-end', 120000).catch((e) =>
            warn(`nova-buddy: 剧本生成失败 ${e?.message || e}`));
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
    flushSessions(false);
  }

  function onSessionDisposed(session) {
    const sid = String(session?.id || '');
    if (sid && sessions.delete(sid)) {
      flushState(true);
      flushSessions(true);
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
    if (closed) return powerStore.flush();
    closed = true;
    if (stateTimer) clearTimeout(stateTimer);
    if (sessionsTimer) clearTimeout(sessionsTimer);
    if (quotaTimer) clearInterval(quotaTimer);
    if (rankTimer) clearInterval(rankTimer);
    if (hbTimer) clearInterval(hbTimer);
    if (choreoTimer) clearInterval(choreoTimer);
    if (tokensTimer) clearInterval(tokensTimer);
    for (const ac of activeFollowAborts) ac.abort();
    activeFollowAborts.clear();
    for (const apId of [...pendingApprovals.keys()]) {
      settleApproval(apId, { kind: 'abort' }, { notifyDeviceEnd: true });
    }
    client?.end?.(true);
    return powerStore.flush();
  }

  return {
    start,
    stop,
    onSessionEvent,
    onSessionDisposed,
    onApprovalRequest,
    buildStatePayload,
    buildSessionsPayload,
    setSessionController,
    syncSessionsFromController,
    refreshQuota,
    refreshRank,
    refreshChoreo,
    refreshTokens,
    getPowerReport: (id) => powerStore.getReport(id),
    flushPower: () => powerStore.flush(),
    powerStore,
    publishTts,
    publishTo,
    onlineIds,
    pushTime,
  };
}
