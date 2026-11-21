import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { STATE_DIR } from './fetchers.js';
import { wavToNvAdpcm } from './adpcm.js';

const CONFIG_FILE = join(STATE_DIR, 'config.json');
const VALID_ANIMS = new Set([
  'idle', 'thinking', 'working', 'waiting', 'success', 'error', 'click', 'sleep', 'quota',
]);

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

export function normalizeChoreo(rawObj) {
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
    if (!Number.isFinite(dur) || dur <= 0) dur = 4000;
    if (dur < 100) dur = Math.round(dur * 1000); // normalize seconds -> ms
    dur = Math.max(1800, Math.min(15000, Math.round(dur)));

    const say = String(s.say || '').replace(/[\r\n]+/g, ' ').trim().slice(0, 18);
    steps.push({ x, speed, anim, dur, say });
  }
  if (steps.length === 0) throw new Error('No valid choreography steps');
  return { t: 'choreo', mood, steps };
}

async function callOneLlm(provider, prompt) {
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
              '请根据当前时间、主人开发状态、算力余额，编排一段 4 到 5 步的循环桌面动画剧本。\n' +
              '严格只输出纯 JSON（禁止 Markdown 代码块或任何解释）：\n' +
              '{"mood":"4-8字心情","steps":[{"x":184,"speed":"walk","anim":"idle","dur":4500,"say":"不超过10字"}]}\n' +
              '约束：x为整数[80,288]；speed仅限"walk"或"run"；anim仅限idle/thinking/working/waiting/success/error/sleep/quota；dur为毫秒整数[2500,10000]；say简短自然，深夜(23:00-06:00)多安排idle/sleep温柔提醒休息。',
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
    return normalizeChoreo(extractJsonObject(content));
  } finally {
    clearTimeout(timer);
  }
}

export async function generateChoreography(ctxSummary, aiCfg, log = () => {}) {
  const nowStr = new Date().toLocaleTimeString('zh-CN', { hour12: false, hour: '2-digit', minute: '2-digit' });
  const prompt = `现在时间：${nowStr}。${ctxSummary}`;

  const providers = [aiCfg?.llm?.primary, aiCfg?.llm?.fallback].filter(Boolean);
  for (const p of providers) {
    try {
      const choreo = await callOneLlm(p, prompt);
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

  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 15000);
  try {
    const res = await fetch(tts.url, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'api-key': tts.apiKey,
      },
      body: JSON.stringify({
        model: tts.model || 'mimo-v2.5-tts',
        messages: [
          { role: 'user', content: opts.style || tts.style },
          { role: 'assistant', content: String(text).slice(0, 120) },
        ],
        audio: {
          format: 'wav',
          voice: opts.voice || tts.voice || '冰糖',
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
