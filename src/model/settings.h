#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>
#include "model/aod_policy.h"
#include "model/battery_model.h"

constexpr const char* NOVA_FW_VER = "0.6.0";

enum class HomePageKind : uint8_t {
  PET   = 0,
  QUOTA = 1,
  RANK  = 2,
};

enum class PowerProfile : uint8_t {
  ADAPTIVE  = 0,  // USB 60fps / Battery 30fps
  LONG_BATT = 1,  // Battery 15fps + aggressive idle
  LIVELY    = 2,  // Always 60fps
};

struct NovaSettings {
  HomePageKind homePage      = HomePageKind::PET;
  PowerProfile powerProfile  = PowerProfile::ADAPTIVE;
  uint8_t      soundPct      = 75;
  uint8_t      brightPct     = 100;                  // 20..100（老 NVS 存 0..4 档，见 settingsMigrateBrightPct）
  bool         useNva        = true;
  bool         ttsEnabled    = true;
  bool         quietHours    = true;
  bool         choreoEnabled = true;
  uint16_t     batSleepSec   = 30;
  uint8_t      chargeLimitPct = 90;                  // 80 / 90 / 100
  ChargeSpeed  chargeSpeed    = ChargeSpeed::AUTO;   // 温和 / 自动 / 快速
  // AOD（熄屏显示）—— 默认只在插电时出现，电池仍按 batSleepSec 熄屏。
  uint8_t      aodMode       = aod::USB;             // 关 / 仅插电 / 常开
  uint16_t     aodIdleSec    = 120;                  // 60 / 120 / 300
  uint8_t      aodNightCap   = 1;                    // 0=一档 1=两档 2=跟随用户
  uint8_t      micGainDb     = 24;                   // 18 / 24 / 30 / 36 / 42
  uint8_t      uiTheme       = 0;                    // 0=深海原版 1=深蓝潮汐（NVS key "theme"）
  char         brokerHost[40] = "192.168.31.15";
  uint16_t     brokerPort    = 1883;
};

static_assert(std::is_standard_layout_v<NovaSettings>);

// 老固件的 NVS "brt" 是 0..4 的五档，新固件存 20..100 的百分比（背光本身是 0..255 连续）。
// 读到 <= 4 就按老档位线性映射；20 以上的值原样返回。读路径只此一处迁移，写路径一律写新格式。
inline uint16_t settingsMigrateBrightPct(uint16_t raw) {
  return (raw <= 4u) ? (uint16_t)(20u + raw * 20u) : raw;
}

// NVS 键名、默认值、约束全部集中在这张表；load/save/prefs 三个遍历者共用。
// brokerHost / brokerPort 是字符串/端口，不进表。
// useNva 是死设置（全库无读点）：不进表，nva 键既不读也不写。
enum class SettingKind : uint8_t { BOOL, U8, U16, CHOICE };

struct SettingDef {
  const char*     key;        // NVS key —— 必须与历史版本逐字相同
                              // toHost 项同时用它作 MQTT prefs 的 JSON 键，改名等于两处一起改
  SettingKind     kind;
  uint16_t        def;
  uint16_t        minV, maxV; // choices 非空时忽略
  const uint16_t* choices;    // 离散合法值，如 {80,90,100}
  uint8_t         nChoices;
  uint16_t        offset;     // offsetof(NovaSettings, ...)
  bool            toHost;     // 是否随 prefs 上宿主
  bool            clampMax = false;  // 越界时夹到 maxV（历史行为：snd/brt），否则回落 def
};

constexpr uint16_t kChoiceHome[]     = { 0, 1, 2 };
constexpr uint16_t kChoicePwr[]      = { 0, 1, 2 };
constexpr uint16_t kChoiceChgLim[]   = { 80, 90, 100 };
constexpr uint16_t kChoiceChgSpd[]   = { 0, 1, 2 };
constexpr uint16_t kChoiceAodMode[]  = { 0, 1, 2 };
constexpr uint16_t kChoiceAodIdle[]  = { 60, 120, 300 };
constexpr uint16_t kChoiceMicGain[]  = { 18, 24, 30, 36, 42 };
constexpr uint16_t kChoiceTheme[]    = { 0, 1 };

constexpr SettingDef kSettings[] = {
  { "home",      SettingKind::CHOICE, 0,   0, 0,   kChoiceHome,    3, offsetof(NovaSettings, homePage),      false },
  { "pwr",       SettingKind::CHOICE, 0,   0, 0,   kChoicePwr,     3, offsetof(NovaSettings, powerProfile),  false },
  { "snd",       SettingKind::U8,     75,  0, 100, nullptr,        0, offsetof(NovaSettings, soundPct),      true,  true },
  { "brt",       SettingKind::U8,     100, 20, 100, nullptr,        0, offsetof(NovaSettings, brightPct),     false, true },
  { "tts",       SettingKind::BOOL,   1,   0, 0,   nullptr,        0, offsetof(NovaSettings, ttsEnabled),    true  },
  { "quiet",     SettingKind::BOOL,   1,   0, 0,   nullptr,        0, offsetof(NovaSettings, quietHours),    true  },
  { "choreo",    SettingKind::BOOL,   1,   0, 0,   nullptr,        0, offsetof(NovaSettings, choreoEnabled), true  },
  { "sleep_s",   SettingKind::U16,    30,  10, 600, nullptr,       0, offsetof(NovaSettings, batSleepSec),   false },
  { "chg_lim",   SettingKind::CHOICE, 90,  0, 0,   kChoiceChgLim,  3, offsetof(NovaSettings, chargeLimitPct), false },
  { "chg_spd",   SettingKind::CHOICE, 1,   0, 0,   kChoiceChgSpd,  3, offsetof(NovaSettings, chargeSpeed),    false },
  { "aod",       SettingKind::CHOICE, 1,   0, 0,   kChoiceAodMode, 3, offsetof(NovaSettings, aodMode),        false },
  { "aod_idle",  SettingKind::U16,    120, 0, 0,   kChoiceAodIdle, 3, offsetof(NovaSettings, aodIdleSec),     false },
  { "aod_night", SettingKind::U8,     1,   0, 2,   nullptr,        0, offsetof(NovaSettings, aodNightCap),   false },
  { "mic_gain",  SettingKind::CHOICE, 24,  0, 0,   kChoiceMicGain, 5, offsetof(NovaSettings, micGainDb),     false },
  { "theme",     SettingKind::CHOICE, 0,   0, 0,   kChoiceTheme,   2, offsetof(NovaSettings, uiTheme),       false },
};

uint16_t settingsFieldGet(const NovaSettings& cfg, const SettingDef& d);
void     settingsFieldSet(NovaSettings& cfg, const SettingDef& d, uint16_t v);
uint16_t settingsSanitize(const SettingDef& d, uint16_t v);

void settingsLoad();
void settingsSave();
// 13 个设置项回出厂值并落盘。配网信息（brokerHost / brokerPort）不在设置表里，保持不动。
void settingsRestoreDefaults();
const NovaSettings& settingsGet();
NovaSettings& settingsMut();
uint8_t settingsEffectiveFps(bool usbPresent);
uint32_t settingsNvsFailCount();   // NVS 打开失败次数
