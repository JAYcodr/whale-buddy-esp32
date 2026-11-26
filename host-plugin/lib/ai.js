import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { STATE_DIR } from './fetchers.js';
import { wavToNvAdpcm } from './adpcm.js';

const CONFIG_FILE = join(STATE_DIR, 'config.json');

// MiMo 声音克隆：audio.voice 收 data URL 而不是音色名（改音色名会 400
// `audio.voice must be a data URL for voice clone model`）。参考音放仓库里，
// 用本地 Qwen3-TTS 现合成的那批预置语音之一。
const CLONE_MODEL = 'mimo-v2.5-tts-voiceclone';
const DEFAULT_VOICE_REF = fileURLToPath(new URL('../assets/voice-ref.wav', import.meta.url));
let cachedRefPath = '';
let cachedRefDataUrl = '';

function resolveVoiceRef(tts) {
  const path = tts?.voiceRef || DEFAULT_VOICE_REF;
  if (!path || !existsSync(path)) return '';
  if (path === cachedRefPath && cachedRefDataUrl) return cachedRefDataUrl;
  try {
    cachedRefDataUrl = `data:audio/wav;base64,${readFileSync(path).toString('base64')}`;
    cachedRefPath = path;
    return cachedRefDataUrl;
  } catch {
    return '';
  }
}
// choreo 动作白名单：顺序 = src/anim/nva.h 的 NvaAnimKey 0..23，
// 也是 src/net/proto.cpp:animNameToIdx 的映射表。三处必须逐项一致，
// 名字对不上设备端会静默回落 idle（proto 里未知动作一律返 0）。
const VALID_ANIMS = new Set([
  'idle', 'thinking', 'working', 'waiting', 'success', 'error', 'click', 'sleep', 'quota',
  'click_happy', 'click_shy', 'click_angry', 'click_giggle',
  'crab_walk', 'happy_run', 'coding', 'archive',
  'stretch', 'yawn', 'wake_up', 'hum', 'eat_token', 'bubble', 'quota_worry',
]);
// 防重复替换池：白天偏元气互动，夜里偏困倦安静。池子前面几项仍是基础动作，
// 老剧本的行为不变；新动作排在后面当「没在用的就换这个」的备选。
const DIVERSE_DAY_POOL = [
  'idle', 'thinking', 'waiting', 'quota', 'success', 'working', 'click',
  'click_happy', 'stretch', 'hum', 'bubble', 'crab_walk', 'happy_run',
  'coding', 'archive', 'eat_token', 'click_shy',
];
const DIVERSE_NIGHT_POOL = [
  'sleep', 'idle', 'waiting', 'thinking', 'quota',
  'yawn', 'stretch', 'hum', 'bubble', 'wake_up',
];

export function loadAiConfig(overrides = {}) {
  let fileCfg = {};
  try {
    if (existsSync(CONFIG_FILE)) {
      fileCfg = JSON.parse(readFileSync(CONFIG_FILE, 'utf8'));
    }
  } catch {
    fileCfg = {};
  }
  return {
    llm: {
      intervalMs: overrides.choreoIntervalMs ?? fileCfg.llm?.intervalMs ?? 15 * 60 * 1000,
      primary: fileCfg.llm?.primary || null,
      fallback: fileCfg.llm?.fallback || null,
    },
    tts: {
      url: fileCfg.tts?.url || 'https://api.xiaomimimo.com/v1/chat/completions',
      apiKey: overrides.ttsApiKey || process.env.MIMO_API_KEY || fileCfg.tts?.apiKey || '',
      model: fileCfg.tts?.model || 'mimo-v2.5-tts',
      voice: overrides.ttsVoice || fileCfg.tts?.voice || '冰糖',
      voiceRef: overrides.ttsVoiceRef || fileCfg.tts?.voiceRef || DEFAULT_VOICE_REF,
      style: overrides.ttsStyle || fileCfg.tts?.style ||
        '用元气可爱、轻快带一点点小傲娇的鲸鱼娘小女仆语气朗读，语速自然清晰。',
      quietHours: overrides.quietHours || fileCfg.tts?.quietHours || [23, 8],
    },
  };
}

export function isQuietHours(quietHours, date = new Date()) {
  if (!Array.isArray(quietHours) || quietHours.length < 2) return false;
  const [startH, endH] = quietHours.map(Number);
  const h = date.getHours();
  if (startH === endH) return false;
  if (startH > endH) return h >= startH || h < endH;
  return h >= startH && h < endH;
}

export function extractJsonObject(rawText) {
  let txt = String(rawText || '').trim();
  txt = txt.replace(/<think>[\s\S]*?<\/think>/gi, '').trim();
  const fence = txt.match(/```(?:json)?\s*([\s\S]*?)```/i);
  if (fence) txt = fence[1].trim();
  const firstBrace = txt.indexOf('{');
  const lastBrace = txt.lastIndexOf('}');
  if (firstBrace < 0 || lastBrace <= firstBrace) {
    throw new Error('No JSON object found in LLM response');
  }
  return JSON.parse(txt.slice(firstBrace, lastBrace + 1));
}

export function normalizeChoreo(rawObj, opts = {}) {
  if (!rawObj || typeof rawObj !== 'object' || !Array.isArray(rawObj.steps)) {
    throw new Error('Missing steps array');
  }
  const mood = String(rawObj.mood || '元气巡游').replace(/\s+/g, ' ').trim().slice(0, 20);
  const steps = [];
  for (const s of rawObj.steps.slice(0, 6)) {
    if (!s || typeof s !== 'object') continue;
    let x = Number(s.x);
    if (!Number.isFinite(x)) x = 184;
    x = Math.max(80, Math.min(288, Math.round(x)));

    let speed = 'walk';
    if (typeof s.speed === 'string') {
      speed = s.speed.toLowerCase().includes('run') ? 'run' : 'walk';
    } else if (typeof s.speed === 'number' && s.speed > 1.1) {
      speed = 'run';
    }

    const rawAnim = String(s.anim || 'idle').toLowerCase().trim();
    const anim = VALID_ANIMS.has(rawAnim) ? rawAnim : 'idle';
    if (anim === 'sleep' || anim === 'yawn') speed = 'walk';

    let dur = Number(s.dur);
    if (!Number.isFinite(dur) || dur <= 0) dur = 11000;
    if (dur < 100) dur = Math.round(dur * 1000); // normalize seconds -> ms
    dur = Math.max(10000, Math.min(24000, Math.round(dur)));

    const say = String(s.say || '').replace(/[\r\n]+/g, ' ').trim().slice(0, 18);
    steps.push({ x, speed, anim, dur, say });
  }
  if (steps.length === 0) throw new Error('No valid choreography steps');

  // Enforce anti-repetition within the loop chain (inspired by aipets ClipSelector + ambient-service):
  // avoid consecutive duplicates (including loop wrap-around) and over-reusing a single animation.
  const pool = opts.night ? DIVERSE_NIGHT_POOL : DIVERSE_DAY_POOL;
  const recentTail = Array.isArray(opts.recentAnims) && opts.recentAnims.length > 0
    ? opts.recentAnims[opts.recentAnims.length - 1]
    : null;
  const counts = new Map();
  for (let i = 0; i < steps.length; i++) {
    const prevAnim = i === 0 ? recentTail : steps[i - 1].anim;
    const nextWrapAnim = (steps.length >= 2 && i === steps.length - 1) ? steps[0].anim : null;
    const cur = steps[i].anim;
    const usedCount = counts.get(cur) || 0;
    const conflictsNeighbor = (cur === prevAnim) || (nextWrapAnim && cur === nextWrapAnim);
    const hasUnusedInPool = pool.some((p) => (counts.get(p) || 0) === 0 && p !== prevAnim && p !== nextWrapAnim);
    if (conflictsNeighbor || (usedCount >= 1 && hasUnusedInPool)) {
      const replacement =
        pool.find((p) => (counts.get(p) || 0) === 0 && p !== prevAnim && p !== nextWrapAnim) ||
        pool.find((p) => p !== prevAnim && p !== nextWrapAnim) ||
        cur;
      steps[i].anim = replacement;
    }
    counts.set(steps[i].anim, (counts.get(steps[i].anim) || 0) + 1);
  }

  return { t: 'choreo', mood, steps };
}

async function requestChatCompletion(provider, messages, { timeoutMs = 25000, maxTokens = 8000 } = {}) {
  if (!provider?.url || !provider?.apiKey || !provider?.model) {
    throw new Error('LLM provider not configured');
  }
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetch(provider.url, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${provider.apiKey}`,
      },
      body: JSON.stringify({
        model: provider.model,
        max_tokens: Number(provider.maxTokens || maxTokens),
        reasoning_effort: provider.reasoningEffort || 'low',
        messages,
      }),
      signal: ctrl.signal,
    });
    if (!res.ok) {
      throw new Error(`HTTP ${res.status}: ${(await res.text()).slice(0, 160)}`);
    }
    const json = await res.json();
    return json?.choices?.[0]?.message?.content || '';
  } finally {
    clearTimeout(timer);
  }
}

async function callOneLlm(provider, prompt, normOpts = {}) {
  const content = await requestChatCompletion(
    provider,
    [
      {
        role: 'system',
        content:
          '你是桌面AI宠物「鲸鱼娘诺瓦」的行为编排导演。人设：元气可爱、轻快带一点点小傲娇的小女仆。\n' +
          '请根据当前时间、主人开发状态、算力余额、今日 Token 消耗与上一轮已播动作，编排一段 5 步左右的循环桌面动画剧本。\n' +
          '可选动作库（每段约10秒，anim 只认下面这 24 个名字）：\n' +
          '【基础状态】idle(悠闲待机)、thinking(思考散步)、working(认真干活)、waiting(乖巧张望)、success(比耶开心)、error(委屈叹气)、sleep(犯困打盹)\n' +
          '【被点回应】click(元气挥手)、click_happy(开心跃动)、click_shy(害羞惊讶)、click_giggle(挠痒咯咯笑)、click_angry(傲娇生气)、wake_up(打瞌睡被惊醒)\n' +
          '【走路步伐】crab_walk(螃蟹横走，配 speed:"walk")、happy_run(开心小跑，配 speed:"run")\n' +
          '【工作感知】coding(写代码)、archive(清点归档)\n' +
          '【日常摸鱼】stretch(超大伸懒腰)、hum(悠闲哼歌)、bubble(鲸鱼吐泡泡)\n' +
          '【余额与干饭】quota(钱袋满溢)、quota_worry(数金皱眉)、eat_token(吃Token)\n' +
          '【困倦预警】yawn(哈欠连天)\n' +
          '严格只输出纯 JSON（禁止 Markdown 代码块或任何解释）：\n' +
          '{"mood":"4-8字心情","steps":[{"x":184,"speed":"walk","anim":"idle","dur":11500,"say":"不超过10字"}]}\n' +
          '编排规则（重要）：\n' +
          '1. 5步内尽量覆盖 4~5 种不同 anim，禁止连续两步相同动画，首步与末步也不得相同；避免单调重复！\n' +
          '2. dur 为毫秒整数 [11000,18000]（每个动作至少11秒以便完整播完一整段动画）。\n' +
          '3. x 为整数 [80,288]（每步换个位置更有灵动感），speed 仅限 "walk" 或 "run"。\n' +
          '4. 语义贴得住才好看：余额紧张用 quota_worry、余额充裕用 quota；今天 Token 烧得多就 eat_token 干饭；主人在调代码用 coding、在翻搜索结果用 archive；日常摸鱼多掺 hum / bubble / stretch / click_happy。\n' +
          '5. 深夜(23:00-06:00)犯困先 yawn 再 sleep，多排 sleep/idle/waiting 并温柔提醒休息；白天多用元气互动动作。\n' +
          '6. click_* 系列与 wake_up 只用于「真的被戳」的语境，平时别排，否则看起来像在自嗨。',
      },
      { role: 'user', content: prompt },
    ],
    { timeoutMs: 25000, maxTokens: 8000 },
  );
  return normalizeChoreo(extractJsonObject(content), normOpts);
}

export async function generateChoreography(ctxSummary, aiCfg, log = () => {}, history = {}) {
  const now = new Date();
  const h = now.getHours();
  const nowStr = now.toLocaleTimeString('zh-CN', { hour12: false, hour: '2-digit', minute: '2-digit' });
  const recentAnims = Array.isArray(history.recentAnims) ? history.recentAnims.slice(-6) : [];
  const recentMood = history.recentMood ? `，上一轮心情：${history.recentMood}` : '';
  const recentStr = recentAnims.length ? `。上一轮动作序列：[${recentAnims.join('→')}]${recentMood}，本轮请换新花样` : '';
  const prompt = `现在时间：${nowStr}。${ctxSummary}${recentStr}`;
  const normOpts = { night: h >= 23 || h < 6, recentAnims };

  const providers = [aiCfg?.llm?.primary, aiCfg?.llm?.fallback].filter(Boolean);
  for (const p of providers) {
    try {
      const choreo = await callOneLlm(p, prompt, normOpts);
      log(`choreo generated by ${p.name || p.model}: ${choreo.mood} (${choreo.steps.length} steps)`);
      return choreo;
    } catch (err) {
      log(`choreo ${p.name || p.model} failed: ${err.message}`);
    }
  }
  return null;
}

// 屏显与 TTS 共用同一套显示宽度上限（40 全角字宽；ASCII 按 0.5 字宽计）。
export const DEVICE_REPLY_WIDTH = 40;

export function measureDisplayWidth(text) {
  let w = 0;
  for (const ch of String(text || '')) {
    const cp = ch.codePointAt(0);
    w += cp <= 0x7f ? 0.5 : 1;
  }
  return w;
}

export function clipByDisplayWidth(text, maxWidth = DEVICE_REPLY_WIDTH) {
  const s = String(text || '').replace(/\s+/g, ' ').trim();
  if (!s) return '';
  if (measureDisplayWidth(s) <= maxWidth) return s;
  let out = '';
  let w = 0;
  const budget = Math.max(1, maxWidth - 1);
  for (const ch of s) {
    const cw = ch.codePointAt(0) <= 0x7f ? 0.5 : 1;
    if (w + cw > budget) break;
    out += ch;
    w += cw;
  }
  return `${out.trimEnd()}…`;
}

export function stripMarkdownToPlain(rawText) {
  let txt = String(rawText || '');
  txt = txt.replace(/<think>[\s\S]*?<\/think>/gi, ' ');
  txt = txt.replace(/```[\s\S]*?```/g, ' ');
  txt = txt.replace(/`([^`]*)`/g, '$1');
  txt = txt.replace(/\*\*([^*]+)\*\*/g, '$1');
  txt = txt.replace(/__([^_]+)__/g, '$1');
  txt = txt.replace(/(^|\s)[#>*\-]+(?=\s)/g, '$1');
  return txt.replace(/[\r\n]+/g, ' ').replace(/\s+/g, ' ').trim();
}

function cleanChatReply(rawText) {
  return clipByDisplayWidth(stripMarkdownToPlain(rawText), DEVICE_REPLY_WIDTH);
}

/**
 * 工作会话出站压缩器：把长篇回复压成适合 1.8 寸屏和语音的一句结论（<=40 全角字宽）。
 * 已在宽度内直接放行；超长时用轻量 LLM 提取结论；不可用/失败时返回明确模板，绝不盲截前 40 字。
 */
export async function summarizeSessionReply(
  { userText = '', assistantText = '', errText = '' } = {},
  aiCfg,
  log = () => {},
) {
  if (errText) {
    return clipByDisplayWidth(`出错：${stripMarkdownToPlain(errText)}`, DEVICE_REPLY_WIDTH);
  }
  const plain = stripMarkdownToPlain(assistantText);
  if (!plain) return '';
  if (measureDisplayWidth(plain) <= DEVICE_REPLY_WIDTH) return plain;

  const tail = plain.length > 1200 ? plain.slice(-1200) : plain;
  const providers = [aiCfg?.llm?.primary, aiCfg?.llm?.fallback].filter(
    (p) => p?.url && p?.apiKey && p?.model,
  );
  for (const p of providers) {
    try {
      const raw = await requestChatCompletion(
        p,
        [
          {
            role: 'system',
            content:
              '你是桌面 1.8 寸副屏的电报压缩器。\n' +
              '请根据用户原话和助手回复，提炼出一句最核心的结果或阻塞点。\n' +
              '严格要求：纯中文自然口语，禁止 Markdown，禁止寒暄过渡词，不超过 38 个汉字。',
          },
          {
            role: 'user',
            content: `用户问：${String(userText || '').slice(0, 120)}\n助手回复：${tail}`,
          },
        ],
        { timeoutMs: 8000, maxTokens: 120 },
      );
      const summary = cleanChatReply(raw);
      if (summary) {
        log(`session reply compressed by ${p.name || p.model}: ${summary}`);
        return summary;
      }
    } catch (err) {
      log(`compress reply ${p.name || p.model} failed: ${err.message}`);
    }
  }
  return '本轮已处理完成，详细结果请看电脑端。';
}

function buildChatFallbackReply(ctxSummary, hadConfiguredProvider) {
  const summary = String(ctxSummary || '');
  const runMatch = summary.match(/DSH运行=(\d+)/);
  const waitMatch = summary.match(/等待=(\d+)/);
  const balMatch = summary.match(/DeepSeek余额([^\s，,]+)/);
  const parts = [];
  if (runMatch) parts.push(`运行${runMatch[1]}任务`);
  if (waitMatch && Number(waitMatch[1]) > 0) parts.push(`待审批${waitMatch[1]}`);
  if (balMatch) parts.push(`余额${balMatch[1]}`);
  const status = parts.join('，');

  if (!hadConfiguredProvider) {
    const base = status ? `已收到（${status}），目前没有配置对话大模型。` : '已收到，目前没有配置对话大模型。';
    return base.length <= 60 ? base : '已收到，目前没有配置对话大模型。';
  }
  const failBase = status ? `诺瓦信号不太稳，当前${status}，稍后再试哦。` : '诺瓦刚刚信号卡了一下，稍后再试一次吧。';
  return failBase.length <= 60 ? failBase : `${failBase.slice(0, 59)}…`;
}

/**
 * 语音对话回复生成：携带实时桌面上下文与最多 6 轮历史，输出纯中文短句（<= 60 字），
 * 供 1.8" OLED 对话气泡与 40 字语音播报使用；未配置或全失败时返回简洁兜底回复。
 */
export async function generateChatReply(userText, history, ctxSummary, aiCfg, log = () => {}) {
  const text = String(userText || '').trim();
  const safeHistory = Array.isArray(history)
    ? history
      .filter((m) => m && (m.role === 'user' || m.role === 'assistant') && typeof m.content === 'string' && m.content.trim())
      .slice(-12)
      .map((m) => ({ role: m.role, content: m.content.trim() }))
    : [];

  const systemPrompt =
    '你是桌面AI伙伴「鲸鱼娘诺瓦」：元气可爱、聪明贴心、带一点点小傲娇。\n' +
    `实时状态：${ctxSummary || '待命中'}\n` +
    '回复规则：\n' +
    '1. 只用纯中文自然口语回答，严禁任何 Markdown 符号（如 *、#、`、- 列表）或代码块。\n' +
    '2. 回答必须精炼在 40 字以内，方便 1.8 寸小屏展示和简短语音播报。';

  const messages = [
    { role: 'system', content: systemPrompt },
    ...safeHistory,
    { role: 'user', content: text },
  ];

  const providers = [aiCfg?.llm?.primary, aiCfg?.llm?.fallback].filter(
    (p) => p?.url && p?.apiKey && p?.model,
  );
  for (const p of providers) {
    try {
      const raw = await requestChatCompletion(p, messages, { timeoutMs: 15000, maxTokens: 512 });
      const reply = cleanChatReply(raw);
      if (!reply) throw new Error('Empty reply');
      log(`chat reply generated by ${p.name || p.model}: ${reply}`);
      return reply;
    } catch (err) {
      log(`chat reply ${p.name || p.model} failed: ${err.message}`);
    }
  }
  return buildChatFallbackReply(ctxSummary, providers.length > 0);
}

export async function synthesizeSpeechAdpcm(text, opts = {}, aiCfg = loadAiConfig()) {
  const tts = aiCfg?.tts;
  if (!tts?.apiKey || !text) return null;
  if (!opts.ignoreQuiet && isQuietHours(tts.quietHours)) return null;

  // 有参考音就走克隆模型（音色跟预置语音同一个人）；没有就退回预设音色。
  const cloneVoice = resolveVoiceRef(tts);
  const model = cloneVoice ? CLONE_MODEL : tts.model || 'mimo-v2.5-tts';
  const voice = cloneVoice || opts.voice || tts.voice || '冰糖';

  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 20000);
  try {
    const res = await fetch(tts.url, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'api-key': tts.apiKey,
      },
      body: JSON.stringify({
        model,
        messages: [
          { role: 'user', content: opts.style || tts.style },
          { role: 'assistant', content: String(text).slice(0, 120) },
        ],
        audio: {
          format: 'wav',
          voice,
        },
        stream: false,
      }),
      signal: ctrl.signal,
    });
    if (!res.ok) throw new Error(`MiMo TTS HTTP ${res.status}`);
    const json = await res.json();
    const b64 = json?.choices?.[0]?.message?.audio?.data;
    if (!b64) throw new Error('MiMo TTS missing audio.data');
    return wavToNvAdpcm(Buffer.from(b64, 'base64'), 16000);
  } finally {
    clearTimeout(timer);
  }
}
