#pragma once
#include <stddef.h>
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
constexpr int MAX_CHAT_MSGS = 8;
constexpr int MAX_BOARD_SESSIONS = 4;
constexpr int MAX_SESS_STEPS = 3;

enum class BoardSessState : uint8_t {
  IDLE = 0,
  RUN  = 1,
  WAIT = 2,
  ERR  = 3,
};

struct BoardStep {
  char time[12];
  char tool[16];
  char arg[40];
};

struct BoardSession {
  char           id[48];
  char           name[44];
  char           cwd[28];
  BoardSessState state;
  uint32_t       elapsedSec;
  char           act[52];
  char           note[64];
  BoardStep      steps[MAX_SESS_STEPS];
  uint8_t        nSteps;
};

enum class ChatDelivKind : uint8_t {
  NONE  = 0,
  SENT  = 1,
  RUN   = 2,
  REPLY = 3,
  FAIL  = 4,
  MUTE  = 5,
};

struct ChatDelivery {
  ChatDelivKind kind;
  uint16_t      id;
  uint8_t       queuePos;
  uint32_t      secs;
  uint32_t      updatedMs;
  char          targetId[48];
  char          targetName[44];
  char          text[160];
};

struct ChatMsg {
  bool     isUser;
  uint16_t id;
  char     text[160];
};

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
  uint8_t  pwrOnSrc;     // AXP2101 PWRON_STATUS (0x20) 开机黑匣子
  uint8_t  pwrOffSrc;    // AXP2101 PWROFF_STATUS (0x21) 关机黑匣子

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

  // Voice recording & WAV upload (CHAT page)
  uint8_t    recState;         // 0=IDLE, 1=RECORDING, 2=UPLOADING, 3=SENT_WAIT_ACK, 4=SAVED, 5=ERR
  uint8_t    recGainDb;        // 18 / 24 / 30 / 36 / 42
  uint8_t    recLevel;         // 0..100 live input meter
  uint8_t    recUploadPct;     // 0..100 MQTT chunk progress
  bool       recHasClip;
  uint16_t   recClipId;
  uint16_t   recSavedCount;
  uint32_t   recDurMs;
  float      recPeakDb;
  float      recRmsDb;
  uint16_t   recClipPermille;
  char       recLastFile[40];
  char       recErr[32];
  uint32_t   recGen;

  // Multi-turn voice chat (CHAT page) & Command Block Mode (Session Board)
  ChatMsg      chatMsgs[MAX_CHAT_MSGS];
  uint8_t      nChatMsgs;
  bool         chatDraftActive;
  uint16_t     chatDraftId;
  uint32_t     chatDraftStartMs;
  char         chatDraftText[160];
  bool         chatWaitingReply;
  uint32_t     chatGen;

  BoardSession boardSessions[MAX_BOARD_SESSIONS];
  uint8_t      nBoardSessions;
  uint32_t     boardRxMs;
  uint32_t     boardGen;
  char         boardTargetId[48];
  char         boardExpandedId[48];
  bool         boardHoldCancel;
  char         boardToast[40];
  uint32_t     boardToastStartMs;
  char         lastSayText[160];
  uint32_t     lastSayAgeSec;
  uint32_t     lastSayRxMs;
  ChatDelivery chatDelivery;
  char         apSessName[44];
  char         apNote[64];
};

void novaStateInit(const char* devId, const char* fwVer);
void novaStateRead(NovaState* out);
void novaSafeUtf8Copy(char* dst, size_t cap, const char* src);
void novaChatAppend(NovaState& st, bool isUser, uint16_t id, const char* text);
const BoardSession* novaBoardFind(const NovaState& st, const char* id);
const BoardSession* novaBoardDefaultTarget(const NovaState& st);
const BoardSession* novaBoardCycleTarget(const NovaState& st, const char* curId);

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
