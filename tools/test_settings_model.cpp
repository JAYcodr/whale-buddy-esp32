// 卡 3 等价性测试的「改动前」基准。
// 这里的 legacyLoad() 逐字复制自 7e35e4d 之前的 settingsLoad()，
// 只把 Arduino Preferences 换成一个最小 FakeNvs（std::map 存储）。
// 下一轮接 tools/test_settings.sh：随机 10,000 组输入，逐字段比对
// legacyLoad() 与新的表驱动 settingsLoad()。
//
// 本文件当前只是基准存档，还没有 main()。

#include "model/settings.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

class FakeNvs {
 public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}

  uint8_t getUChar(const char* key, uint8_t def) const {
    auto it = num_.find(key);
    return (it == num_.end()) ? def : static_cast<uint8_t>(it->second);
  }
  uint16_t getUShort(const char* key, uint16_t def) const {
    auto it = num_.find(key);
    return (it == num_.end()) ? def : static_cast<uint16_t>(it->second);
  }
  bool getBool(const char* key, bool def) const {
    auto it = num_.find(key);
    return (it == num_.end()) ? def : (it->second != 0);
  }
  const char* getString(const char* key, const char* def) const {
    auto it = str_.find(key);
    return (it == str_.end()) ? def : it->second.c_str();
  }

  void setNum(const char* key, uint32_t v) { num_[key] = v; }
  void setStr(const char* key, const char* v) { str_[key] = v; }

  std::map<std::string, uint32_t> num_;
  std::map<std::string, std::string> str_;
};

static void legacyLoad(NovaSettings& s_cfg, const FakeNvs& prefs) {
  uint8_t hp = prefs.getUChar("home", 0);
  s_cfg.homePage = (hp <= 2) ? (HomePageKind)hp : HomePageKind::PET;
  uint8_t pp = prefs.getUChar("pwr", 0);
  s_cfg.powerProfile = (pp <= 2) ? (PowerProfile)pp : PowerProfile::ADAPTIVE;
  s_cfg.soundPct = prefs.getUChar("snd", 75);
  if (s_cfg.soundPct > 100) s_cfg.soundPct = 100;
  s_cfg.brightLevel = prefs.getUChar("brt", 4);
  if (s_cfg.brightLevel > 4) s_cfg.brightLevel = 4;
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
  const char* host = prefs.getString("mqtt_host", "192.168.31.15");
  std::strncpy(s_cfg.brokerHost, host, sizeof(s_cfg.brokerHost) - 1);
  s_cfg.brokerHost[sizeof(s_cfg.brokerHost) - 1] = '\0';
  uint16_t port = prefs.getUShort("mqtt_port", 1883);
  s_cfg.brokerPort = (port > 0) ? port : 1883;
}
