import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { dirname, join } from 'node:path';

export const STATE_DIR = join(homedir(), '.dsh', 'nova-buddy');
const CACHE_FILE = join(STATE_DIR, 'cache.json');
const OPENDESIGN_COOKIE_FILE = join(STATE_DIR, 'opendesign.cookie');
const DEEPSEEK_KEY_FILE = join(STATE_DIR, 'deepseek.key');

const MODELDIAL_TTL_MS = 6 * 60 * 60 * 1000;
const FETCH_TIMEOUT_MS = 8000;

function ensureDir(dir) {
  mkdirSync(dir, { recursive: true, mode: 0o700 });
}

export function saveOpenDesignCookie(rawCookie) {
  ensureDir(STATE_DIR);
  const cleaned = String(rawCookie || '').trim();
  const value = cleaned.includes('=')
    ? cleaned
    : `__Secure-better-auth.session_token=${cleaned}`;
  writeFileSync(OPENDESIGN_COOKIE_FILE, `${value}\n`, { mode: 0o600 });
  return OPENDESIGN_COOKIE_FILE;
}

export function loadCache() {
  try {
    if (!existsSync(CACHE_FILE)) return { quota: {}, rank: null };
    return JSON.parse(readFileSync(CACHE_FILE, 'utf8'));
  } catch {
    return { quota: {}, rank: null };
  }
}

export function saveCache(cache) {
  try {
    ensureDir(STATE_DIR);
    const tmp = `${CACHE_FILE}.tmp`;
    writeFileSync(tmp, JSON.stringify(cache, null, 2), { mode: 0o600 });
    renameSync(tmp, CACHE_FILE);
  } catch {
    // Non-fatal cache persistence failure
  }
}

async function fetchJsonWithRetry(url, init = {}, retries = 2) {
  let lastErr;
  for (let i = 0; i <= retries; i++) {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), FETCH_TIMEOUT_MS);
    try {
      const res = await fetch(url, { ...init, signal: ctrl.signal });
      clearTimeout(timer);
      if (res.status === 401 || res.status === 403) {
        const err = new Error(`HTTP ${res.status}`);
        err.status = res.status;
        throw err;
      }
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return await res.json();
    } catch (err) {
      clearTimeout(timer);
      lastErr = err;
      if (err?.status === 401 || err?.status === 403) throw err;
    }
  }
  throw lastErr;
}

function resolveDeepSeekKey(credentialsService) {
  if (process.env.DEEPSEEK_API_KEY) return process.env.DEEPSEEK_API_KEY.trim();
  try {
    const k = credentialsService?.get?.('DEEPSEEK_API_KEY') || credentialsService?.get?.('deepseek');
    if (typeof k === 'string' && k.trim()) return k.trim();
  } catch { /* ignore */ }
  try {
    if (existsSync(DEEPSEEK_KEY_FILE)) {
      const k = readFileSync(DEEPSEEK_KEY_FILE, 'utf8').trim();
      if (k) return k;
    }
  } catch { /* ignore */ }
  try {
    const credPath = join(homedir(), '.dsh', '.credentials.yaml');
    if (existsSync(credPath)) {
      const text = readFileSync(credPath, 'utf8');
      const m = text.match(/(?:DEEPSEEK_API_KEY|deepseek_api_key|deepseek)\s*:\s*["']?([^\s"'\n]+)/i);
      if (m?.[1]) return m[1].trim();
    }
  } catch { /* ignore */ }
  return '';
}

function resolveOpenCodeKey() {
  try {
    const p = join(homedir(), '.local', 'share', 'opencode', 'auth.json');
    if (!existsSync(p)) return '';
    const json = JSON.parse(readFileSync(p, 'utf8'));
    return String(json?.['opencode-go']?.key || json?.opencodeGo?.key || '').trim();
  } catch {
    return '';
  }
}

function resolveOpenDesignCookie() {
  try {
    if (!existsSync(OPENDESIGN_COOKIE_FILE)) return '';
    return readFileSync(OPENDESIGN_COOKIE_FILE, 'utf8').trim();
  } catch {
    return '';
  }
}

function secondsUntil(isoOrEpoch) {
  if (!isoOrEpoch) return 0;
  if (typeof isoOrEpoch === 'number') {
    return isoOrEpoch > 1e12
      ? Math.max(0, Math.round((isoOrEpoch - Date.now()) / 1000))
      : Math.max(0, Math.round(isoOrEpoch > 1e9 ? isoOrEpoch - Date.now() / 1000 : isoOrEpoch));
  }
  const ms = Date.parse(String(isoOrEpoch));
  return Number.isFinite(ms) ? Math.max(0, Math.round((ms - Date.now()) / 1000)) : 0;
}

export async function fetchDeepSeekCard(cache, credentialsService) {
  const key = resolveDeepSeekKey(credentialsService);
  const prev = cache.quota?.ds;
  if (!key) {
    return prev
      ? { ...prev.card, age: Math.round((Date.now() - prev.at) / 1000) }
      : { id: 'ds', name: 'DeepSeek', kind: 'na', why: 'no-key', age: 0 };
  }
  try {
    const data = await fetchJsonWithRetry('https://api.deepseek.com/user/balance', {
      headers: { Authorization: `Bearer ${key}`, Accept: 'application/json' },
    });
    const info = Array.isArray(data?.balance_infos) ? data.balance_infos[0] : null;
    const val = Number(info?.total_balance ?? data?.total_balance ?? 0);
    const cur = String(info?.currency || 'CNY');
    const card = { id: 'ds', name: 'DeepSeek', kind: 'bal', val, cur, age: 0 };
    cache.quota.ds = { at: Date.now(), card };
    return card;
  } catch (err) {
    if (err?.status === 401 || err?.status === 403) {
      return { id: 'ds', name: 'DeepSeek', kind: 'na', why: 'reauth', age: 0 };
    }
    return prev
      ? { ...prev.card, age: Math.round((Date.now() - prev.at) / 1000) }
      : { id: 'ds', name: 'DeepSeek', kind: 'na', why: 'fetch-err', age: 0 };
  }
}

export async function fetchOpenCodeCard(cache) {
  const key = resolveOpenCodeKey();
  const prev = cache.quota?.oc;
  if (!key) {
    return prev
      ? { ...prev.card, age: Math.round((Date.now() - prev.at) / 1000) }
      : { id: 'oc', name: 'OpenCode Go', kind: 'na', why: 'no-key', age: 0 };
  }
  try {
    const data = await fetchJsonWithRetry('https://opencode.ai/zen/go/v1/usage', {
      headers: { Authorization: `Bearer ${key}`, Accept: 'application/json' },
    });
    const u = data?.usage || data || {};
    const wObj = u.weekly || u.weeklyUsage || u.windows?.weekly || {};
    const mObj = u.monthly || u.monthlyUsage || u.windows?.monthly || {};
    const wPct = Math.round(Number(wObj.percent ?? wObj.usagePercent ?? wObj.usedPct ?? 0));
    const mPct = Math.round(Number(mObj.percent ?? mObj.usagePercent ?? mObj.usedPct ?? 0));
    const wRst = secondsUntil(wObj.resetsAt ?? wObj.resetAt ?? wObj.resetInSec ?? 0);
    const mRst = secondsUntil(mObj.resetsAt ?? mObj.resetAt ?? mObj.resetInSec ?? 0);
    const card = {
      id: 'oc',
      name: 'OpenCode Go',
      kind: 'win',
      bars: [
        [wPct, 'w', wRst],
        [mPct, 'm', mRst],
      ],
      age: 0,
    };
    cache.quota.oc = { at: Date.now(), card };
    return card;
  } catch (err) {
    if (err?.status === 401 || err?.status === 403) {
      return { id: 'oc', name: 'OpenCode Go', kind: 'na', why: 'reauth', age: 0 };
    }
    return prev
      ? { ...prev.card, age: Math.round((Date.now() - prev.at) / 1000) }
      : { id: 'oc', name: 'OpenCode Go', kind: 'na', why: 'fetch-err', age: 0 };
  }
}

export async function fetchOpenDesignCard(cache) {
  const cookie = resolveOpenDesignCookie();
  const prev = cache.quota?.od;
  if (!cookie) {
    return { id: 'od', name: 'OpenDesign', kind: 'na', why: 'no-session', age: 0 };
  }
  try {
    const ws = await fetchJsonWithRetry('https://amr-api.open-design.ai/api/v1/workspaces/current', {
      headers: { Cookie: cookie, Accept: 'application/json' },
    });
    const wsId = String(ws?.workspaceId || ws?.activeWorkspaceId || ws?.id || ws?.data?.id || '');
    if (!wsId) throw new Error('missing workspace id');

    const usage = await fetchJsonWithRetry(
      `https://amr-api.open-design.ai/api/v1/workspaces/${encodeURIComponent(wsId)}/billing/coding-plan-usage`,
      {
        headers: {
          Cookie: cookie,
          'X-Vela-Workspace-Id': wsId,
          Accept: 'application/json',
        },
      },
    );
    const uData = usage?.data || usage || {};
    const bars = [];
    if (Array.isArray(uData.windows) && uData.windows.length > 0) {
      for (const win of uData.windows.slice(0, 2)) {
        const used = Number(win.usedCredits ?? 0);
        const limit = Number(win.limitCredits ?? 0);
        const pct = limit > 0
          ? Math.round((used / limit) * 100)
          : Math.round(Number(win.percent ?? 0));
        const dur = Number(win.durationSeconds ?? 0);
        const tag = dur <= 21600 ? '5h' : (dur <= 604800 ? 'w' : 'm');
        bars.push([Math.max(0, Math.min(100, pct)), tag, secondsUntil(win.resetsAt ?? win.resetAt ?? 0)]);
      }
    } else {
      const primaryPct = Math.round(Number(uData.usagePercent ?? uData.percent ?? uData.usedPercent ?? 0));
      bars.push([primaryPct, '5h', secondsUntil(uData.resetAt ?? uData.resetsAt ?? 0)]);
      if (uData.weeklyPercent != null || uData.weekly?.percent != null) {
        bars.push([
          Math.round(Number(uData.weeklyPercent ?? uData.weekly?.percent ?? 0)),
          'w',
          secondsUntil(uData.weeklyResetAt ?? uData.weekly?.resetAt ?? 0),
        ]);
      }
    }
    const card = { id: 'od', name: 'OpenDesign', kind: 'win', bars, age: 0 };
    cache.quota.od = { at: Date.now(), card };
    return card;
  } catch (err) {
    if (err?.status === 401 || err?.status === 403) {
      return { id: 'od', name: 'OpenDesign', kind: 'na', why: 'no-session', age: 0 };
    }
    return prev
      ? { ...prev.card, age: Math.round((Date.now() - prev.at) / 1000) }
      : { id: 'od', name: 'OpenDesign', kind: 'na', why: 'fetch-err', age: 0 };
  }
}

export async function fetchAllQuota(credentialsService) {
  const cache = loadCache();
  cache.quota ||= {};
  const [ds, oc, od] = await Promise.all([
    fetchDeepSeekCard(cache, credentialsService),
    fetchOpenCodeCard(cache),
    fetchOpenDesignCard(cache),
  ]);
  saveCache(cache);
  return { t: 'quota', cards: [ds, oc, od] };
}

function stripHtml(s) {
  return String(s || '')
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/<[^>]*>/g, ' ')
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&nbsp;/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}

function compactDuration(raw) {
  const m = String(raw || '').match(/(\d+)\s*m/);
  if (m) return `${m[1]}m`;
  const s = String(raw || '').match(/(\d+)\s*s/);
  if (s) return `${s[1]}s`;
  return String(raw || '').slice(0, 8);
}

function parseModelDialHtml(html) {
  const rows = [];
  const rowRe = /<tr\b[^>]*\bdata-model="([^"]+)"[^>]*>([\s\S]*?)<\/tr>/g;
  let m;
  while ((m = rowRe.exec(html)) !== null) {
    const body = m[2];
    const tds = [...body.matchAll(/<td\b[^>]*>[\s\S]*?<\/td>/g)].map((x) => x[0]);
    const nameMatch = body.match(/<a[^>]*class="model-result-link"[^>]*>([\s\S]*?)<\/a>/);
    const effortMatch = body.match(/<span class="effort-label">([\s\S]*?)<\/span>/);
    const scores = [];
    for (const td of tds) {
      if (!/class="[^"]*\bscore\b/.test(td)) continue;
      const val = td.match(/<span>\s*([\d.]+)\s*<\/span>/);
      if (val) scores.push(Number(val[1]));
    }
    const durationMatch = body.match(/<td[^>]*class="duration-cell"[^>]*>([\s\S]*?)<\/td>/);
    const costMatch = body.match(/<td[^>]*class="cost-cell"[^>]*>([\s\S]*?)<\/td>/);
    const name = nameMatch ? stripHtml(nameMatch[1]) : stripHtml(tds[1] || '');
    if (!name || scores.length === 0) continue;
    rows.push({
      r: Number(stripHtml(tds[0] || '')) || (rows.length + 1),
      m: name.slice(0, 26),
      e: (effortMatch ? stripHtml(effortMatch[1]) : '').slice(0, 8),
      n: /<span class="model-new-badge"(?![^>]*\bhidden\b)/.test(body) ? 1 : 0,
      s: Number(Number(scores[0] ?? 0).toFixed(1)),
      b: Math.round(scores[1] ?? 0),
      a: Math.round(scores[2] ?? 0),
      k: Math.round(scores[3] ?? 0),
      f: Math.round(scores[4] ?? 0),
      q: Math.round(scores[5] ?? 0),
      d: compactDuration(durationMatch ? stripHtml(durationMatch[1]) : ''),
      c: (costMatch ? stripHtml(costMatch[1]) : '').slice(0, 8),
    });
  }
  return rows;
}

export async function fetchModelDialRank(force = false) {
  const cache = loadCache();
  if (!force && cache.rank?.at && Date.now() - cache.rank.at < MODELDIAL_TTL_MS) {
    return cache.rank.payload;
  }
  try {
    let rows = [];
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 15000);
    try {
      const htmlRes = await fetch('https://modeldial.com/leaderboard/coding?axis=overall', {
        headers: {
          Accept: 'text/html,application/xhtml+xml',
          'Accept-Language': 'zh-CN,zh;q=0.9,en;q=0.8',
          'User-Agent': 'deepseek-harness/modeldial-leaderboard',
        },
        signal: ctrl.signal,
      });
      if (htmlRes.ok) {
        rows = parseModelDialHtml(await htmlRes.text()).slice(0, 24);
      }
    } catch {
      // Fall back to radar JSON endpoint below.
    } finally {
      clearTimeout(timer);
    }

    if (rows.length === 0) {
      const raw = await fetchJsonWithRetry('https://modeldial.com/api/v1/radar/latest.json', {
        headers: { Accept: 'application/json' },
      });
      const list = Array.isArray(raw)
        ? raw
        : (raw?.overallRankings || raw?.rankings || raw?.models || raw?.rows || raw?.data || []);
      rows = list.slice(0, 24).map((item, idx) => ({
        r: Number(item.rank ?? idx + 1),
        m: String(item.model ?? item.name ?? item.id ?? '').slice(0, 26),
        e: String(item.reasoningEffort ?? item.effort ?? item.reasoning ?? '').slice(0, 8),
        n: 0,
        s: Number(Number(item.overallScore ?? item.score ?? item.overall ?? 0).toFixed(1)),
        b: Math.round(Number(item.backendScore ?? item.backend ?? item.scores?.backend ?? 0)),
        f: Math.round(Number(item.frontendScore ?? item.frontend ?? item.scores?.frontend ?? 0)),
        k: Math.round(Number(item.knowledgeScore ?? item.knowledge ?? item.scores?.knowledge ?? 0)),
        a: 0,
        q: 0,
        d: item.elapsedMs ? `${Math.round(Number(item.elapsedMs) / 60000)}m` : '',
        c: item.estimatedReferenceCostUsd ? `$${Number(item.estimatedReferenceCostUsd).toFixed(1)}` : '',
      }));
    }

    const payload = {
      t: 'rank',
      src: 'ModelDial',
      lic: 'CC BY 4.0',
      at: Math.floor(Date.now() / 1000),
      rows,
    };
    cache.rank = { at: Date.now(), payload };
    saveCache(cache);
    return payload;
  } catch {
    return cache.rank?.payload || null;
  }
}
