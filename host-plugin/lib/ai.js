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
const VALID_ANIMS = new Set([
  'idle', 'thinking', 'working', 'waiting', 'success', 'error', 'click', 'sleep', 'quota',
]);
const DIVERSE_DAY_POOL = ['idle', 'thinking', 'waiting', 'quota', 'success', 'working', 'click'];
const DIVERSE_NIGHT_POOL = ['sleep', 'idle', 'waiting', 'thinking', 'quota'];

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

async function callOneLlm(provider, prompt, normOpts = {}) {
  if (!provider?.url || !provider?.apiKey || !provider?.model) {
    throw new Error('LLM provider not configured');
  }
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 25000);
  try {
    const res = await fetch(provider.url, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${provider.apiKey}`,
      },
      body: JSON.stringify({
        model: provider.model,
        max_tokens: Number(provider.maxTokens || 8000),
        reasoning_effort: provider.reasoningEffort || 'low',
        messages: [
          {
            role: 'system',
            content:
              '你是桌面AI宠物「鲸鱼娘诺瓦」的行为编排导演。人设：元气可爱、轻快带一点点小傲娇的小女仆。\n' +
              '请根据当前时间、主人开发状态、算力余额与上一轮已播动作，编排一段 5 步左右的循环桌面动画剧本。\n' +
              '可选动作库（每段约10秒）：idle(悠闲待机)、thinking(思考散步)、working(认真干活)、waiting(乖巧张望)、success(比耶开心)、quota(看余额算力)、click(元气挥手)、sleep(犯困打盹)、error(委屈叹气)。\n' +
              '严格只输出纯 JSON（禁止 Markdown 代码块或任何解释）：\n' +
              '{"mood":"4-8字心情","steps":[{"x":184,"speed":"walk","anim":"idle","dur":11500,"say":"不超过10字"}]}\n' +
              '编排规则（重要）：\n' +
              '1. 5步内尽量覆盖 4~5 种不同 anim，禁止连续两步相同动画，首步与末步也不得相同；避免单调重复！\n' +
              '2. dur 为毫秒整数 [11000,18000]（每个动作至少11秒以便完整播完一整段动画）。\n' +
              '3. x 为整数 [80,288]（每步换个位置更有灵动感），speed 仅限 "walk" 或 "run"。\n' +
              '4. 深夜(23:00-06:00)多安排 sleep/idle/waiting 并温柔提醒休息；白天多用元气互动动作。',
          },
          { role: 'user', content: prompt },
        ],
      }),
      signal: ctrl.signal,
    });
    if (!res.ok) {
      throw new Error(`HTTP ${res.status}: ${(await res.text()).slice(0, 160)}`);
    }
    const json = await res.json();
    const content = json?.choices?.[0]?.message?.content || '';
    return normalizeChoreo(extractJsonObject(content), normOpts);
  } finally {
    clearTimeout(timer);
  }
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
