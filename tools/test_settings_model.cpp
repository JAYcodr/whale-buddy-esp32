// 卡 3 等价性测试：改动前的 settingsLoad() vs 表驱动的新实现。
//
// legacyLoad() 逐字复制自 7b848d3 之前的 settingsLoad()，只把内部使用的
// Preferences 换成 tools/test_stubs 的假 NVS（同样是类型化的 getter 语义）。
// 新实现走 settings.cpp 里的 settingsLoad()/settingsSave()。
//
// Build/run: bash tools/test_settings.sh

#include "model/settings.h"
#include "Preferences.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
  g_checks++; \
  if (!(cond)) { \
    g_fail++; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
  } \
} while (0)

static void legacyLoad(NovaSettings& s_cfg, const Preferences& prefs) {
  uint8_t hp = prefs.getUChar("home", 0);
  s_cfg.homePage = (hp <= 2) ? (HomePageKind)hp : HomePageKind::PET;
  uint8_t pp = prefs.getUChar("pwr", 0);
  s_cfg.powerProfile = (pp <= 2) ? (PowerProfile)pp : PowerProfile::ADAPTIVE;
  s_cfg.soundPct = prefs.getUChar("snd", 75);
  if (s_cfg.soundPct > 100) s_cfg.soundPct = 100;
  // brt 老格式 0..4 档 → 20..100 百分比；合法区间外一律回出厂 100。
  uint16_t brt = prefs.getUChar("brt", 100);
  if (brt <= 4) brt = (uint16_t)(20u + brt * 20u);
  if (brt < 20) brt = 100;
  if (brt > 100) brt = 100;
  s_cfg.brightPct = (uint8_t)brt;
  s_cfg.useNva = prefs.getBool("nva", true);
  s_cfg.ttsEnabled = prefs.getBool("tts", true);
  s_cfg.quietHours = prefs.getBool("quiet", true);
  s_cfg.choreoEnabled = prefs.getBool("choreo", true);
  uint16_t slp = prefs.getUShort("sleep_s", 30);
  s_cfg.batSleepSec = (slp >= 10 && slp <= 600) ? slp : 30;
  uint8_t lim = prefs.getUChar("chg_lim", 90);
  s_cfg.chargeLimitPct = (lim == 80 || lim == 90 || lim == 100) ? lim : 90;
  uint8_t spd = prefs.getUChar("chg_spd", (uint8_t)ChargeSpeed::AUTO);
  s_cfg.chargeSpeed = (spd <= (uint8_t)ChargeSpeed::FAST)
                        ? (ChargeSpeed)spd : ChargeSpeed::AUTO;
  uint8_t aodm = prefs.getUChar("aod", aod::USB);
  s_cfg.aodMode = (aodm <= aod::ALWAYS) ? aodm : aod::USB;
  uint16_t aodIdle = prefs.getUShort("aod_idle", 120);
  s_cfg.aodIdleSec = (aodIdle == 60 || aodIdle == 120 || aodIdle == 300) ? aodIdle : 120;
  uint8_t aodNight = prefs.getUChar("aod_night", 1);
  s_cfg.aodNightCap = (aodNight <= 2) ? aodNight : 1;
  uint8_t micGain = prefs.getUChar("mic_gain", 24);
  s_cfg.micGainDb = (micGain == 18 || micGain == 24 || micGain == 30 ||
                     micGain == 36 || micGain == 42) ? micGain : 24;
  String host = prefs.getString("mqtt_host", "192.168.31.15");
  std::strncpy(s_cfg.brokerHost, host.c_str(), sizeof(s_cfg.brokerHost) - 1);
  s_cfg.brokerHost[sizeof(s_cfg.brokerHost) - 1] = '\0';
  uint16_t port = prefs.getUShort("mqtt_port", 1883);
  s_cfg.brokerPort = (port > 0) ? port : 1883;
}

static const char* diffField(const NovaSettings& a, const NovaSettings& b) {
  if (a.homePage != b.homePage) return "homePage";
  if (a.powerProfile != b.powerProfile) return "powerProfile";
  if (a.soundPct != b.soundPct) return "soundPct";
  if (a.brightPct != b.brightPct) return "brightPct";
  if (a.ttsEnabled != b.ttsEnabled) return "ttsEnabled";
  if (a.quietHours != b.quietHours) return "quietHours";
  if (a.choreoEnabled != b.choreoEnabled) return "choreoEnabled";
  if (a.batSleepSec != b.batSleepSec) return "batSleepSec";
  if (a.chargeLimitPct != b.chargeLimitPct) return "chargeLimitPct";
  if (a.chargeSpeed != b.chargeSpeed) return "chargeSpeed";
  if (a.aodMode != b.aodMode) return "aodMode";
  if (a.aodIdleSec != b.aodIdleSec) return "aodIdleSec";
  if (a.aodNightCap != b.aodNightCap) return "aodNightCap";
  if (a.micGainDb != b.micGainDb) return "micGainDb";
  if (std::strcmp(a.brokerHost, b.brokerHost) != 0) return "brokerHost";
  if (a.brokerPort != b.brokerPort) return "brokerPort";
  return nullptr;
}

static const uint32_t kValuePool[] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 29, 30, 59, 60, 74, 75, 76, 79, 80,
  89, 90, 99, 100, 101, 119, 120, 121, 254, 255, 256, 299, 300, 301,
  599, 600, 601, 65534, 65535, 70000, 300000, 0xFFFFFFFFu,
};

static const char* kStrPool[] = {
  "", "0", "80", "1883", "abc", "192.168.1.2", "192.168.31.15",
  "300000", "Nova-FFFFFFFFFFFFFFFF",
  "0123456789012345678901234567890123456789012345678901234567890123456789",
};

static const char* kAllKeys[] = {
  "home", "pwr", "snd", "brt", "tts", "quiet", "choreo", "sleep_s",
  "chg_lim", "chg_spd", "aod", "aod_idle", "aod_night", "mic_gain",
  "nva", "mqtt_host", "mqtt_port",
};

static void randomStore(std::mt19937& rng) {
  prefs_stub::clear();
  for (const char* key : kAllKeys) {
    if ((rng() % 100) < 45) continue;            // 键不存在
    const uint32_t v = kValuePool[rng() % (sizeof(kValuePool) / sizeof(kValuePool[0]))];
    switch (rng() % 6) {
      case 0: prefs_stub::setU8(key, (uint8_t)v); break;
      case 1: prefs_stub::setU16(key, (uint16_t)v); break;
      case 2: prefs_stub::setU32(key, v); break;
      case 3: prefs_stub::setI8(key, (int8_t)v); break;
      case 4:
        prefs_stub::setStr(key, kStrPool[rng() % (sizeof(kStrPool) / sizeof(kStrPool[0]))]);
        break;
      default: prefs_stub::setU8(key, (uint8_t)(rng() % 2)); break;   // 布尔形态
    }
  }
}

static bool sameEntry(const prefs_stub::Entry& a, const prefs_stub::Entry& b) {
  return a.type == b.type && a.num == b.num && a.str == b.str;
}

static bool storeKeyAllowed(const std::string& k) {
  for (const char* allowed : kAllKeys) {
    if (k == allowed) return true;
  }
  return false;
}

int main() {
  // 默认值：空 NVS 下新实现必须落在表里的 def。
  prefs_stub::clear();
  settingsLoad();
  const NovaSettings& d = settingsGet();
  CHECK(d.homePage == HomePageKind::PET);
  CHECK(d.powerProfile == PowerProfile::ADAPTIVE);
  CHECK(d.soundPct == 75);
  CHECK(d.brightPct == 100);
  CHECK(d.ttsEnabled && d.quietHours && d.choreoEnabled);
  CHECK(d.batSleepSec == 30);
  CHECK(d.chargeLimitPct == 90);
  CHECK(d.chargeSpeed == ChargeSpeed::AUTO);
  CHECK(d.aodMode == aod::USB);
  CHECK(d.aodIdleSec == 120);
  CHECK(d.aodNightCap == 1);
  CHECK(d.micGainDb == 24);
  CHECK(std::strcmp(d.brokerHost, "192.168.31.15") == 0);
  CHECK(d.brokerPort == 1883);

  constexpr int kIters = 10000;
  std::mt19937 rng(20261123u);
  int mismatchIters = 0;

  for (int i = 0; i < kIters; i++) {
    randomStore(rng);

    // 旧实现
    NovaSettings legacy{};
    {
      Preferences prefs;
      prefs.begin("nova", true);
      legacyLoad(legacy, prefs);
      prefs.end();
    }

    // 新实现
    settingsLoad();
    NovaSettings driven = settingsGet();

    const char* bad = diffField(legacy, driven);
    if (bad) {
      mismatchIters++;
      std::printf("MISMATCH iter=%d field=%s\n", i, bad);
      if (mismatchIters >= 5) break;
      continue;
    }

    // save：nva 必须原样不动；表里的键类型/值正确；不产生新键。
    const prefs_stub::Entry nvaBefore = prefs_stub::get("nva");
    settingsSave();
    if (!sameEntry(nvaBefore, prefs_stub::get("nva"))) {
      mismatchIters++;
      std::printf("MISMATCH iter=%d nva changed by settingsSave\n", i);
      break;
    }
    for (const SettingDef& def : kSettings) {
      const prefs_stub::Entry e = prefs_stub::get(def.key);
      const uint16_t v = settingsFieldGet(driven, def);
      const prefs_stub::T wantType =
          (def.kind == SettingKind::U16) ? prefs_stub::T::U16 : prefs_stub::T::U8;
      if (e.type != wantType || e.num != v) {
        mismatchIters++;
        std::printf("MISMATCH iter=%d save key=%s type=%u num=%u want=%u\n",
                    i, def.key, (unsigned)e.type, (unsigned)e.num, (unsigned)v);
        break;
      }
    }
    if (mismatchIters) break;
    for (const auto& kv : prefs_stub::store()) {
      if (!storeKeyAllowed(kv.first)) {
        mismatchIters++;
        std::printf("MISMATCH iter=%d unexpected key=%s\n", i, kv.first.c_str());
        break;
      }
    }
    if (mismatchIters) break;
  }

  CHECK(mismatchIters == 0);
  if (mismatchIters == 0) {
    std::printf("%d groups identical (load/save/defaults)\n", kIters);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
