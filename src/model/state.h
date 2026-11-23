#pragma once
#include <stdint.h>

struct QuotaBar {
  uint8_t  pct;
  char     tag[4];
  uint32_t resetSec;
};

struct QuotaCard {
  char     id[8];
  char     name[24];
  char     kind[8];  // "bal" | "win" | "na"
  double   val;
  char     cur[8];
  char     why[24];
  uint32_t ageSec;
  QuotaBar bars[2];
  uint8_t  nBars;
};

constexpr int MAX_RANK_ROWS = 24;
constexpr int MAX_CHOREO_STEPS = 6;
constexpr int MAX_TOKEN_DAYS = 14;

struct TokenDay {
  char     date[8];
  uint64_t tokens;
};

struct ChoreoStep {
  int16_t  x;
  bool     run;
  uint8_t  anim;   // NvaAnimKey index 0..8
  uint16_t durMs;
  char     say[64];
};

struct RankRow {
  uint8_t r;
  char    m[28];
  char    e[10];
  bool    isNew;
  float   s;
  uint8_t b;
  uint8_t f;
  uint8_t k;
  uint8_t a;
  uint8_t q;
  char    dur[10];
  char    cost[10];
};

struct NovaState {
  // Identity & network
  char     deviceId[16];
  char     fwVer[16];
  char     ip[20];
  char     apSsid[24];
  int8_t   rssi;
  bool     wifiConnected;
  bool     wifiApMode;
  bool     mqttConnected;
  uint32_t lastMqttRxMs;

  // DSH live state
  uint16_t run;
  uint16_t wait;
  uint16_t sess;
  char     msg[112];
  bool     turnError;
  int8_t   ctxPct;
  char     activeModel[32];
  // 主屏码头的扩展信息（state 载荷平级字段，不塞进 err）
  char     sessTitle[40];   // 当前会话标题
  char     lastTool[24];    // 上一个工具名
  char     errMsg[136];     // 本轮异常原文，供展开查看
  bool     dockExpanded;    // 异常详情是否展开（纯本地 UI 状态）
  uint32_t stateRxMs;
  uint32_t stateGen;

  // Quota cards
  QuotaCard cards[3];
  uint8_t   nCards;
  uint32_t  quotaRxMs;
  uint32_t  quotaGen;

  // ModelDial rank
  char     rankSrc[24];
  char     rankLic[16];
  uint32_t rankAt;
  RankRow  rows[MAX_RANK_ROWS];
  uint8_t  nRows;
  uint32_t rankRxMs;
  uint32_t rankGen;

  // Approval
  bool     approvalActive;
  char     apId[40];
  char     apTool[32];
  char     apReason[160];
  uint32_t apTtlMs;
  uint32_t apArrivedMs;
  uint32_t apGen;

  // Device telemetry
  float    fps;
  uint32_t pushUs;
  uint32_t droppedFps;
  uint32_t freeHeap;
  uint32_t freePsram;
  uint16_t battMv;
  uint8_t  battPct;
  bool     usbPresent;
  bool     charging;
  uint8_t  battStage;       // BattStage (model/battery_model.h)
  int8_t   battTempC;       // AXP2101 die temperature
  uint16_t chgMa;           // charge current requested by the governor
  bool     chargeLimited;   // paused by limit / temperature
  bool     chargeTempCapped;// paused specifically due to die temperature
  int8_t   gaugePct;        // AXP2101 gauge SOC, -1 when unavailable
  bool     faceDown;
  uint8_t  hour;
  uint8_t  minute;
  uint8_t  second;
  uint8_t  month;      // AOD 日期行
  uint8_t  day;
  uint8_t  dow;        // 0=周日
  bool     timeValid;
  uint32_t nowMs;
  uint16_t cpuMhz;
  uint8_t  burnInStep;

  // v0.5.0 观测 / AOD
  float    drawFps;      // 最近一次「有效绘制窗口」的帧率（静帧保持上次值）
  uint32_t lastDrawMs;   // 最近一次推送的时间戳
  uint32_t drawCount;    // 累计推送次数（探针算绘制/分钟）
  uint8_t  pwrProfile;   // PowerProfile 快照
  uint8_t  effBright;    // 当前生效的背光百分比 20..100
  bool     lcdOff;       // 熄屏 / 暗屏
  bool     aodActive;    // 正在显示 AOD
  uint8_t  uiBusyPct;    // UI 任务忙时占比（1s 窗口）
  uint8_t  decBusyPct;   // 解码任务忙时占比（1s 窗口）
  int8_t   txPower;      // 当前 WiFi 最大发射功率（0.25dBm 单位，80=20dBm）

  // LLM choreography loop
  char       choreoMood[32];
  ChoreoStep choreoSteps[MAX_CHOREO_STEPS];
  uint8_t    nChoreoSteps;
  uint32_t   choreoGen;

  // Daily token consumption (whale-school page)
  TokenDay   tokenDays[MAX_TOKEN_DAYS];
  uint8_t    nTokenDays;
  uint64_t   tokenTotal;
  uint64_t   tokenAvgOverride;
  uint16_t   tokenSpanDays;
  float      tokenCachePct;
  uint32_t   tokenRxMs;
  uint32_t   tokenGen;
};

void novaStateInit(const char* devId, const char* fwVer);
void novaStateRead(NovaState* out);

template <typename Fn>
void novaStateWithLock(Fn&& fn);

// Non-template lock helpers used by novaStateWithLock
void novaStateLock();
void novaStateUnlock();
NovaState& novaStateUnsafeRef();

template <typename Fn>
void novaStateWithLock(Fn&& fn) {
  novaStateLock();
  fn(novaStateUnsafeRef());
  novaStateUnlock();
}
