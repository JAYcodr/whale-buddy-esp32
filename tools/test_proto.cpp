// Host-side tests for src/net/proto.cpp（协议编解码 + 载荷预算 + TTS 分片）。
// Build/run: bash tools/test_proto.sh
//
// proto.cpp 故意**不**定义 NOVA_HOST_SIM：这样才能把真实的 applyTtsChunk
// 分片状态机编进来。ESP 侧的 heap_caps / mbedtls / bus / audio 符号由
// tools/test_stubs 和本文件的桩提供。

#include "net/proto.h"
#include "model/aod_policy.h"
#include "model/battery_model.h"
#include "model/state.h"
#include "hw/audio.h"
#include "hw/bus.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// 已知缺陷的「期望行为」断言：默认不计入回归，避免把主回归钉在红灯上。
// 复现：NOVA_EXPECT_IDEAL=1 bash tools/test_proto.sh
// 修好实现后，把对应 IDEAL_CHECK 改成 CHECK 并删掉钉死现状的那条。
#define IDEAL_CHECK(cond) do { \
  if (std::getenv("NOVA_EXPECT_IDEAL") != nullptr) { CHECK(cond); } \
} while (0)

// 校验截断后的字符串仍是合法 UTF-8（不能出现半个汉字 / 断裂续字节）。
static bool validUtf8(const char* s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    size_t n = (*p < 0x80) ? 1
             : ((*p & 0xE0) == 0xC0) ? 2
             : ((*p & 0xF0) == 0xE0) ? 3
             : ((*p & 0xF8) == 0xF0) ? 4 : 0;
    if (!n) return false;
    for (size_t i = 1; i < n; i++) {
      if ((p[i] & 0xC0) != 0x80) return false;
    }
    p += n;
  }
  return true;
}

// ── ESP 侧符号的桩 ────────────────────────────────────────────────
bool hwBusRequestRtcWrite(const HwTime&) { return true; }

void hwPlayAdpcmOwned(uint8_t* data, size_t, uint8_t) { free(data); }

static int b64val(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int mbedtls_base64_decode(unsigned char* dst, size_t dlen, size_t* olen,
                          const unsigned char* src, size_t slen) {
  size_t n = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < slen; i++) {
    const unsigned char c = src[i];
    if (c == '=') break;
    if (c == '\n' || c == '\r') continue;
    const int v = b64val(c);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= dlen) return -2;
      dst[n++] = (unsigned char)(acc >> bits);
    }
  }
  *olen = n;
  return 0;
}

// ── 工具 ──────────────────────────────────────────────────────────
static bool stateMsg(const char* json) {
  novaStateInit("testdev", "0.5.0");
  return protoApplyMessage("state", json, std::strlen(json), 1000);
}

static bool stateTurnError(const char* json) {
  if (!stateMsg(json)) return false;
  NovaState st;
  novaStateRead(&st);
  return st.turnError;
}

// 负例必须证明消息真的被应用了，而不是 protoApplyMessage 直接拒了。
static bool appliedWithoutError(const char* json) {
  return stateMsg(json) && !stateTurnError(json);
}

static bool appliedWithError(const char* json) {
  return stateMsg(json) && stateTurnError(json);
}

static bool ttsMsg(const char* json) {
  return protoApplyMessage("tts", json, std::strlen(json), 2000);
}

static NovaState worstCaseState() {
  NovaState st{};
  st.fps = 100000.0f;
  st.drawFps = 100000.0f;
  st.droppedFps = 0xFFFFFFFFu;
  st.freeHeap = 0xFFFFFFFFu;
  st.freePsram = 0xFFFFFFFFu;
  st.cpuMhz = 0xFFFFu;
  st.rssi = -128;
  st.battMv = 0xFFFFu;
  st.battPct = 0xFFu;
  st.charging = true;
  st.battStage = 0xFFu;
  st.battTempC = -128;
  st.pushUs = 0xFFFFFFFFu;
  st.nowMs = 0xFFFFFFFFu;
  st.lastDrawMs = 0;
  st.drawCount = 0xFFFFFFFFu;
  st.pwrProfile = 0xFFu;
  st.effBright = 0xFFu;
  st.lcdOff = true;
  st.aodActive = true;
  st.txPower = -128;
  st.uiBusyPct = 0xFFu;
  st.decBusyPct = 0xFFu;
  return st;
}

int main() {
  char buf[512];

  // ── 1. err 只认字面 true（ArduinoJson 的 `| false` 语义）──────────
  CHECK(appliedWithError("{\"run\":0,\"err\":true}"));
  CHECK(appliedWithoutError("{\"run\":0,\"err\":1}"));
  CHECK(appliedWithoutError("{\"run\":0,\"err\":\"true\"}"));
  CHECK(appliedWithoutError("{\"run\":0,\"err\":{\"a\":1}}"));
  CHECK(appliedWithoutError("{\"run\":0,\"err\":false}"));
  CHECK(appliedWithoutError("{\"run\":5,\"err\":true}"));        // 有任务在跑就不算本轮异常
  CHECK(appliedWithError("{\"run\":0,\"msg\":\"这里异常\"}")); // msg 兜底
  CHECK(appliedWithoutError("{\"run\":0,\"msg\":\"运行中\"}"));

  // ── 2. stat / hello / decision / prefs 的截断必须返回 0 ──────────
  const NovaState worst = worstCaseState();
  const NovaSettings& cfg = settingsGet();

  size_t n = protoBuildStat(buf, sizeof(buf), worst);
  CHECK(n > 0);
  CHECK(n < 320);
  CHECK(std::strlen(buf) == n);            // 正常路径必须写 NUL

  CHECK(protoBuildStat(buf, 16, worst) == 0);      // 截断 → 明确失败
  CHECK(protoBuildStat(buf, 1, worst) == 0);
  CHECK(protoBuildHello(buf, 8, worst, cfg, true) == 0);
  CHECK(protoBuildDecision(buf, 8, "approval-id", true, false) == 0);
  CHECK(protoBuildPrefs(buf, 1, cfg) == 0);

  n = protoBuildStat(buf, 320, worst);
  CHECK(n > 0 && n < 320);
  n = protoBuildHello(buf, sizeof(buf), worst, cfg, true);
  CHECK(n > 0 && std::strlen(buf) == n);
  n = protoBuildDecision(buf, sizeof(buf), "approval-id", true, false);
  CHECK(n > 0 && std::strlen(buf) == n);
  n = protoBuildPrefs(buf, sizeof(buf), cfg);
  CHECK(n > 0 && std::strlen(buf) == n);

  // hello 有 4 个字符串字段；最坏字符串 + 320B 缓冲必须还能装下。
  NovaState hs{};
  std::strcpy(hs.deviceId, "FFFFFFFFFFFFFF");
  std::strcpy(hs.fwVer, "0.5.0-rc.12345");
  std::strcpy(hs.ip, "255.255.255.255");
  std::strcpy(hs.apSsid, "Nova-FFFFFFFFFFFFFFFF");
  hs.pwrOnSrc = 255;
  hs.pwrOffSrc = 255;
  NovaSettings hc{};
  hc.soundPct = 255;
  n = protoBuildHello(buf, 320, hs, hc, true);
  CHECK(n > 0 && n < 320);
  CHECK(std::strlen(buf) == n);
  CHECK(std::strstr(buf, "\"onSrc\":255") != nullptr);
  CHECK(std::strstr(buf, "\"offSrc\":255") != nullptr);

  // prefs 只上宿主 toHost 的 4 项，键名与旧实现一致。
  n = protoBuildPrefs(buf, sizeof(buf), cfg);
  CHECK(std::strstr(buf, "\"evt\":\"prefs\"") != nullptr);
  CHECK(std::strstr(buf, "\"snd\":") != nullptr);
  CHECK(std::strstr(buf, "\"tts\":") != nullptr);
  CHECK(std::strstr(buf, "\"quiet\":") != nullptr);
  CHECK(std::strstr(buf, "\"choreo\":") != nullptr);
  CHECK(std::strstr(buf, "\"brt\"") == nullptr);

  // ── 3. tts 分片：错序 / 重复 / seq>=tot 必须被拒 ─────────────────
  CHECK(!ttsMsg("{\"id\":1,\"seq\":0,\"tot\":0,\"b64\":\"AA==\"}"));
  CHECK(!ttsMsg("{\"id\":1,\"seq\":2,\"tot\":3,\"b64\":\"AA==\"}"));   // 先到 seq=2
  CHECK(!ttsMsg("{\"id\":1,\"seq\":0,\"tot\":3,\"b64\":\"\"}"));
  CHECK(!ttsMsg("{\"id\":1,\"seq\":3,\"tot\":3,\"b64\":\"AA==\"}"));   // seq >= tot

  CHECK(ttsMsg("{\"id\":1,\"seq\":0,\"tot\":3,\"b64\":\"AAAA\"}"));    // 正常起头
  CHECK(ttsMsg("{\"id\":1,\"seq\":1,\"tot\":3,\"b64\":\"AAAA\"}"));
  CHECK(!ttsMsg("{\"id\":1,\"seq\":1,\"tot\":3,\"b64\":\"AAAA\"}"));   // 重复
  CHECK(ttsMsg("{\"id\":1,\"seq\":0,\"tot\":3,\"b64\":\"AAAA\"}"));    // 重新起头
  CHECK(ttsMsg("{\"id\":1,\"seq\":1,\"tot\":3,\"b64\":\"AAAA\"}"));
  CHECK(ttsMsg("{\"id\":1,\"seq\":2,\"tot\":3,\"b64\":\"AAAA\"}"));    // 收尾

  CHECK(!ttsMsg("{\"id\":2,\"seq\":1,\"tot\":2,\"b64\":\"AAAA\"}"));   // 新 id 从 1 起
  CHECK(ttsMsg("{\"id\":2,\"seq\":0,\"tot\":2,\"b64\":\"AAAA\"}"));
  CHECK(!ttsMsg("{\"id\":3,\"seq\":1,\"tot\":2,\"b64\":\"AAAA\"}"));   // 中途换 id
  CHECK(ttsMsg("{\"id\":3,\"seq\":0,\"tot\":2,\"b64\":\"AAAA\"}"));
  CHECK(!ttsMsg("{\"id\":3,\"seq\":2,\"tot\":3,\"b64\":\"AAAA\"}"));   // 跳号
  CHECK(ttsMsg("{\"id\":3,\"seq\":0,\"tot\":2,\"b64\":\"AAAA\"}"));    // 失败后可重来

  // ── 4. 静音时段口径（语音 23–8）──────────────────────────────────
  CHECK(!aod::isQuietHour(22));
  CHECK(aod::isQuietHour(23));
  CHECK(aod::isQuietHour(0));
  CHECK(aod::isQuietHour(7));
  CHECK(!aod::isQuietHour(8));

  // ── 5. 电源关键帧载荷（evt:"pwr"，严守 NetTxMsg::payload[448]）────
  {
    char pwrBuf[512] = {};
    PowerEvent boot{};
    boot.reason = BattPwrEvtReason::BOOT;
    boot.seq = 1;
    boot.pct = 74;
    boot.rawPct = 71;
    boot.gaugePct = 68;
    boot.seedPct = 74;
    boot.stage = 2;
    boot.prevStage = 2;
    boot.usbPresent = true;
    boot.charging = true;
    boot.mv = 4152;
    boot.compMv = 3966;
    boot.restMv = 3972;
    boot.vbusMv = 5080;
    boot.vsysMv = 4210;
    boot.chgMa = 300;
    boot.effMa = 300;
    boot.tempC = 33;
    boot.pwrOnSrc = 0x04;
    boot.pwrOffSrc = 0x08;
    boot.chgMohm = 620;
    boot.designMah = 600;
    size_t bn = protoBuildPowerEvent(pwrBuf, sizeof(pwrBuf), boot);
    CHECK(bn > 0 && bn < 448);
    CHECK(std::strstr(pwrBuf, "\"evt\":\"pwr\"") != nullptr);
    CHECK(std::strstr(pwrBuf, "\"why\":\"boot\"") != nullptr);
    CHECK(std::strstr(pwrBuf, "\"onSrc\":4") != nullptr);
    CHECK(std::strstr(pwrBuf, "\"offSrc\":8") != nullptr);
    CHECK(std::strstr(pwrBuf, "\"rmv\":3972") != nullptr);
    CHECK(std::strstr(pwrBuf, "\"vb\":5080") != nullptr);
    CHECK(std::strstr(pwrBuf, "\"vs\":4210") != nullptr);

    PowerEvent maxEv{};
    maxEv.reason = BattPwrEvtReason::BOOT;
    maxEv.seq = 4294967295u;
    maxEv.pct = 100;
    maxEv.rawPct = 100;
    maxEv.gaugePct = 100;
    maxEv.seedPct = 100;
    maxEv.seedPowerLost = true;
    maxEv.stage = 5;
    maxEv.prevStage = 2;
    maxEv.usbPresent = true;
    maxEv.charging = true;
    maxEv.limited = true;
    maxEv.tempCapped = true;
    maxEv.thermReg = true;
    maxEv.vbusLimited = true;
    maxEv.mv = 4250;
    maxEv.compMv = 4250;
    maxEv.restMv = 4205;
    maxEv.vbusMv = 5250;
    maxEv.vsysMv = 4350;
    maxEv.chgMa = 500;
    maxEv.effMa = 500;
    maxEv.tempC = -40;
    maxEv.pwrOnSrc = 255;
    maxEv.pwrOffSrc = 255;
    maxEv.chgMohm = 1500;
    maxEv.designMah = 1200;
    maxEv.hasWatermark = true;
    maxEv.durSec = 864000u;
    maxEv.startPct = 100;
    maxEv.vbatMinMv = 2850;
    maxEv.vbatMaxMv = 4250;
    maxEv.vsysMinMv = 3300;
    maxEv.vbusMinMv = 4350;
    maxEv.tmpMaxC = -40;
    maxEv.vbusLimSec = 65535u;
    maxEv.thermRegSec = 65535u;
    maxEv.tempCapSec = 65535u;
    maxEv.screenOnSec = 65535u;
    maxEv.aodSec = 65535u;
    size_t mn = protoBuildPowerEvent(pwrBuf, sizeof(pwrBuf), maxEv);
    CHECK(mn > 0 && mn < 448);
    CHECK(protoBuildPowerEvent(pwrBuf, 96, maxEv) == 0);
  }

  // ── 6. 录音回传确认载荷（mic_ack）与编号守卫 ─────────────────────
  {
    novaStateInit("testdev", "0.5.0");
    // 未开始录音（recClipId == 0）时，旧回执直接丢弃。
    const char* okAck = "{\"id\":101,\"ok\":true,\"file\":\"rec_001_24dB_3.4s.wav\",\"missing\":0}";
    CHECK(!protoApplyMessage("mic_ack", okAck, std::strlen(okAck), 2900));

    novaStateWithLock([](NovaState& s) {
      s.recState = 3;
      s.recClipId = 101;
      s.recWaitStartMs = 2950;
    });
    CHECK(protoApplyMessage("mic_ack", okAck, std::strlen(okAck), 3000));
    NovaState st;
    novaStateRead(&st);
    CHECK(st.recState == 4);
    CHECK(st.recWaitStartMs == 3000);
    CHECK(st.recSavedCount == 1);
    CHECK(std::strcmp(st.recLastFile, "rec_001_24dB_3.4s.wav") == 0);
    CHECK(st.recErr[0] == '\0');

    // Duplicate clipId does not double-count recSavedCount.
    CHECK(!protoApplyMessage("mic_ack", okAck, std::strlen(okAck), 3100));
    novaStateRead(&st);
    CHECK(st.recSavedCount == 1);

    // 编号不一致的旧回执不得覆盖当前状态。
    const char* staleAck = "{\"id\":99,\"ok\":false,\"missing\":1}";
    CHECK(!protoApplyMessage("mic_ack", staleAck, std::strlen(staleAck), 3150));
    novaStateRead(&st);
    CHECK(st.recState == 4);

    novaStateWithLock([](NovaState& s) {
      s.recState = 3;
      s.recClipId = 102;
    });
    const char* badAck = "{\"id\":102,\"ok\":false,\"missing\":3}";
    CHECK(protoApplyMessage("mic_ack", badAck, std::strlen(badAck), 3200));
    novaStateRead(&st);
    CHECK(st.recState == 5);
    CHECK(std::strstr(st.recErr, "3") != nullptr);

    // 宿主解析结束包失败时发出 id=0, ok=false：等待回执态应收下错误态。
    novaStateWithLock([](NovaState& s) {
      s.recState = 3;
      s.recClipId = 103;
      s.recErr[0] = '\0';
    });
    const char* parseFailAck = "{\"id\":0,\"ok\":false,\"missing\":0,\"err\":\"结束包解析失败\"}";
    CHECK(protoApplyMessage("mic_ack", parseFailAck, std::strlen(parseFailAck), 3300));
    novaStateRead(&st);
    CHECK(st.recState == 5);
    CHECK(std::strcmp(st.recErr, "结束包解析失败") == 0);

    // 超时兜底：状态 3 / 4 超过 10s 落到错误态 5。
    novaStateWithLock([](NovaState& s) {
      s.recState = 3;
      s.recClipId = 104;
      s.recWaitStartMs = 1000;
      s.recErr[0] = '\0';
      CHECK(!novaRecCheckWaitTimeout(s, 10999));
      CHECK(s.recState == 3);
      CHECK(novaRecCheckWaitTimeout(s, 11000));
      CHECK(s.recState == 5);
      CHECK(std::strcmp(s.recErr, "回执超时") == 0);

      s.recState = 4;
      s.recWaitStartMs = 2000;
      s.recErr[0] = '\0';
      CHECK(novaRecCheckWaitTimeout(s, 12000));
      CHECK(s.recState == 5);
      CHECK(std::strcmp(s.recErr, "识别超时") == 0);
    });
  }

  // ── 7. 多轮对话协议（chat asr/reply + chat_send/chat_tts 构造）───
  {
    novaStateInit("testdev", "0.5.0");
    NovaState st;
    novaStateRead(&st);
    CHECK(st.recGainDb == 24);
    CHECK(st.nChatMsgs == 0);
    CHECK(!st.chatDraftActive);

    // 7a. ASR 成功（先验证未录音/旧编号会被丢弃）
    const char* asrOk = "{\"t\":\"asr\",\"id\":42,\"ok\":true,\"text\":\"查一下当前状态\",\"ms\":610}";
    CHECK(!protoApplyMessage("chat", asrOk, std::strlen(asrOk), 3900));
    novaStateWithLock([](NovaState& s) {
      s.recState = 4;
      s.recClipId = 42;
      s.recWaitStartMs = 3950;
    });
    CHECK(protoApplyMessage("chat", asrOk, std::strlen(asrOk), 4000));
    novaStateRead(&st);
    CHECK(st.recState == 0);
    CHECK(st.recWaitStartMs == 0);
    CHECK(st.chatDraftActive);
    CHECK(st.chatDraftId == 42);
    CHECK(st.chatDraftStartMs == 4000);
    CHECK(std::strcmp(st.chatDraftText, "查一下当前状态") == 0);
    CHECK(st.recErr[0] == '\0');
    CHECK(st.recGen == 1);
    CHECK(st.chatGen == 1);

    // 7a-2. 迟到的旧 ASR 不得覆盖正在录的新句子或已有草稿
    novaStateWithLock([](NovaState& s) {
      s.chatDraftActive = false;
      s.chatDraftText[0] = '\0';
      s.recState = 1;
      s.recClipId = 45;
    });
    CHECK(!protoApplyMessage("chat", asrOk, std::strlen(asrOk), 4050));
    novaStateRead(&st);
    CHECK(st.recState == 1);
    CHECK(!st.chatDraftActive);

    // 7b. ASR 空文本 / 失败
    novaStateWithLock([](NovaState& s) {
      s.recState = 4;
      s.recClipId = 43;
      s.chatDraftActive = false;
    });
    const char* asrEmpty = "{\"t\":\"asr\",\"id\":43,\"ok\":true,\"text\":\"   \"}";
    CHECK(protoApplyMessage("chat", asrEmpty, std::strlen(asrEmpty), 4100));
    novaStateRead(&st);
    CHECK(st.recState == 5);
    CHECK(!st.chatDraftActive);
    CHECK(std::strcmp(st.recErr, "未识别到语音") == 0);

    novaStateWithLock([](NovaState& s) {
      s.recState = 4;
      s.recClipId = 44;
    });
    const char* asrFail = "{\"t\":\"asr\",\"id\":44,\"ok\":false,\"err\":\"识别超时\"}";
    CHECK(protoApplyMessage("chat", asrFail, std::strlen(asrFail), 4200));
    novaStateRead(&st);
    CHECK(st.recState == 5);
    CHECK(std::strcmp(st.recErr, "识别超时") == 0);

    // 7c. Reply 成功 + 队列左移（超过 MAX_CHAT_MSGS = 8）
    novaStateWithLock([](NovaState& s) {
      s.recState = 0;
      s.chatWaitingReply = true;
      for (int i = 0; i < MAX_CHAT_MSGS; i++) {
        char msg[24];
        std::snprintf(msg, sizeof(msg), "msg-%d", i);
        novaChatAppend(s, (i % 2) == 0, (uint16_t)(i + 1), msg);
      }
    });
    const char* replyOk = "{\"t\":\"reply\",\"id\":99,\"ok\":true,\"text\":\"当前一切正常\"}";
    CHECK(protoApplyMessage("chat", replyOk, std::strlen(replyOk), 4300));
    novaStateRead(&st);
    CHECK(!st.chatWaitingReply);
    CHECK(st.nChatMsgs == MAX_CHAT_MSGS);
    CHECK(std::strcmp(st.chatMsgs[0].text, "msg-1") == 0);
    CHECK(!st.chatMsgs[MAX_CHAT_MSGS - 1].isUser);
    CHECK(st.chatMsgs[MAX_CHAT_MSGS - 1].id == 99);
    CHECK(std::strcmp(st.chatMsgs[MAX_CHAT_MSGS - 1].text, "当前一切正常") == 0);
    CHECK(st.recErr[0] == '\0');

    // 7d. Reply 失败
    novaStateWithLock([](NovaState& s) { s.chatWaitingReply = true; });
    const char* replyFail = "{\"t\":\"reply\",\"id\":100,\"ok\":false,\"err\":\"回复超时\"}";
    CHECK(protoApplyMessage("chat", replyFail, std::strlen(replyFail), 4400));
    novaStateRead(&st);
    CHECK(!st.chatWaitingReply);
    CHECK(st.recState == 5);
    CHECK(std::strcmp(st.recErr, "回复超时") == 0);

    // 7e. protoBuildChatSend / protoBuildChatTts（含引号与 UTF-8 + 448B 容量）
    char chatBuf[448] = {};
    const char* tricky = "请看 \"quote\" 和 \\slash\n还有中文";
    size_t csLen = protoBuildChatSend(chatBuf, sizeof(chatBuf), 123, tricky);
    CHECK(csLen > 0 && csLen < sizeof(chatBuf));
    CHECK(std::strstr(chatBuf, "\"evt\":\"chat_send\"") != nullptr);
    CHECK(std::strstr(chatBuf, "\"id\":123") != nullptr);
    CHECK(std::strstr(chatBuf, "\\\"quote\\\"") != nullptr);
    CHECK(protoBuildChatSend(chatBuf, 12, 123, tricky) == 0);

    size_t ctLen = protoBuildChatTts(chatBuf, sizeof(chatBuf), tricky);
    CHECK(ctLen > 0 && ctLen < sizeof(chatBuf));
    CHECK(std::strstr(chatBuf, "\"evt\":\"chat_tts\"") != nullptr);
    CHECK(std::strstr(chatBuf, "\\\"quote\\\"") != nullptr);
    CHECK(protoBuildChatTts(chatBuf, 12, tricky) == 0);

    // 159-byte UTF-8 text fits within NetTxMsg::payload[448].
    char maxText[160];
    std::memset(maxText, 'a', 159);
    maxText[159] = '\0';
    CHECK(protoBuildChatSend(chatBuf, 448, 65535, maxText) > 0);
    CHECK(protoBuildChatTts(chatBuf, 448, maxText) > 0);
  }

  // ── 8. 命令方块模式：sessions 看板 + 自动锁目标 + delivery 回执 ───
  {
    novaStateInit("testdev", "0.5.0");
    const char* sessJson =
      "{\"t\":\"sessions\",\"lastSay\":\"单测通过\",\"lastAgo\":120,"
      "\"items\":["
        "{\"id\":\"s2\",\"name\":\"审批会话\",\"cwd\":\"nova\",\"st\":\"wait\",\"el\":15,\"act\":\"bash rm\",\"note\":\"删文件\"},"
        "{\"id\":\"s1\",\"name\":\"电量四改\",\"cwd\":\"nova-buddy\",\"st\":\"run\",\"el\":252,\"act\":\"cargo test\",\"note\":\"2 个失败\","
          "\"steps\":[{\"t\":\"4:02\",\"s\":\"bash\",\"a\":\"cargo test\"}]},"
        "{\"id\":\"s3\",\"name\":\"DS-Bot\",\"cwd\":\"ds-bot\",\"st\":\"idle\",\"el\":480,\"act\":\"已停下\",\"note\":\"8分钟前\"}"
      "]}";
    CHECK(protoApplyMessage("sessions", sessJson, std::strlen(sessJson), 5000));
    NovaState st;
    novaStateRead(&st);
    CHECK(st.nBoardSessions == 3);
    CHECK(st.boardSessions[0].state == BoardSessState::WAIT);
    CHECK(st.boardSessions[1].state == BoardSessState::RUN);
    CHECK(st.boardSessions[1].nSteps == 1);
    CHECK(std::strcmp(st.lastSayText, "单测通过") == 0);
    CHECK(st.lastSayAgeSec == 120);

    // 未手动选目标时，ASR 成功应跳过 WAIT，优先锁定 IDLE 会话 (s3)
    novaStateWithLock([](NovaState& s) {
      s.recState = 4;
      s.recClipId = 7;
    });
    const char* asrOk = "{\"t\":\"asr\",\"id\":7,\"ok\":true,\"text\":\"继续弄\"}";
    CHECK(protoApplyMessage("chat", asrOk, std::strlen(asrOk), 5100));
    novaStateRead(&st);
    CHECK(st.chatDraftActive);
    CHECK(std::strcmp(st.boardTargetId, "s3") == 0);

    // 若用户原本就指向 s1 (RUN)，ASR 后保持用户选择不覆盖
    novaStateWithLock([](NovaState& s) {
      std::strcpy(s.boardTargetId, "s1");
      s.chatDraftActive = false;
      s.recState = 4;
      s.recClipId = 7;
    });
    CHECK(protoApplyMessage("chat", asrOk, std::strlen(asrOk), 5200));
    novaStateRead(&st);
    CHECK(std::strcmp(st.boardTargetId, "s1") == 0);

    // 若用户原本指向 wait 会话 s2，ASR 后自动改锁可用会话
    novaStateWithLock([](NovaState& s) {
      std::strcpy(s.boardTargetId, "s2");
      s.chatDraftActive = false;
      s.recState = 4;
      s.recClipId = 7;
    });
    CHECK(protoApplyMessage("chat", asrOk, std::strlen(asrOk), 5300));
    novaStateRead(&st);
    CHECK(std::strcmp(st.boardTargetId, "s3") == 0);

    // delivery: sent -> run -> reply -> mute -> fail
    const char* dSent = "{\"t\":\"delivery\",\"id\":7,\"kind\":\"sent\",\"pos\":1,\"target\":\"s1\",\"name\":\"电量四改\"}";
    CHECK(protoApplyMessage("chat", dSent, std::strlen(dSent), 5400));
    novaStateRead(&st);
    CHECK(st.chatWaitingReply);
    CHECK(st.chatDelivery.kind == ChatDelivKind::SENT);
    CHECK(st.chatDelivery.queuePos == 1);

    const char* dReply = "{\"t\":\"delivery\",\"id\":7,\"kind\":\"reply\",\"target\":\"s1\",\"text\":\"还剩 2 个断言失败\"}";
    CHECK(protoApplyMessage("chat", dReply, std::strlen(dReply), 5500));
    novaStateRead(&st);
    CHECK(!st.chatWaitingReply);
    CHECK(st.chatDelivery.kind == ChatDelivKind::REPLY);
    CHECK(std::strcmp(st.lastSayText, "还剩 2 个断言失败") == 0);

    const char* dMute = "{\"t\":\"delivery\",\"id\":8,\"kind\":\"mute\",\"target\":\"s1\"}";
    CHECK(protoApplyMessage("chat", dMute, std::strlen(dMute), 5600));
    novaStateRead(&st);
    CHECK(!st.chatWaitingReply);
    CHECK(st.chatDelivery.kind == ChatDelivKind::MUTE);

    char chatBuf[448] = {};
    size_t n = protoBuildChatSend(chatBuf, sizeof(chatBuf), 88, "跑测试", "s1", "steer");
    CHECK(n > 0 && n < sizeof(chatBuf));
    CHECK(std::strstr(chatBuf, "\"target\":\"s1\"") != nullptr);
    CHECK(std::strstr(chatBuf, "\"mode\":\"steer\"") != nullptr);
  }

  // ── 9. 工具审批：下行报文 / 上行决策 / 状态机幂等 ──────────────────
  // 对应 host-plugin/test/approval.test.mjs（宿主侧）。这里只管设备侧纯逻辑。
  {
    const char* apJson = "{\"t\":\"approval\",\"id\":\"ap_7f3\",\"tool\":\"Bash\","
                         "\"reason\":\"rm -rf\",\"ttl\":20000}";
    novaStateInit("testdev", "0.5.0");
    CHECK(protoApplyMessage("approval", apJson, std::strlen(apJson), 1000));
    NovaState st;
    novaStateRead(&st);
    CHECK(st.approvalActive);
    CHECK(std::strcmp(st.apId, "ap_7f3") == 0);
    CHECK(std::strcmp(st.apTool, "Bash") == 0);
    CHECK(std::strcmp(st.apReason, "rm -rf") == 0);
    CHECK(st.apTtlMs == 20000);
    CHECK(st.apArrivedMs == 1000);
    CHECK(st.apGen == 1);

    // 缺 id 必须拒收，不能弹出无主弹窗。
    const char* noId = "{\"t\":\"approval\",\"tool\":\"Bash\"}";
    CHECK(!protoApplyMessage("approval", noId, std::strlen(noId), 1100));
    novaStateRead(&st);
    CHECK(std::strcmp(st.apId, "ap_7f3") == 0);

    // 同 id 重投必须幂等：倒计时不重置、不重播到达音 / 转场，但仍刷新存活时间。
    CHECK(protoApplyMessage("approval", apJson, std::strlen(apJson), 5000));
    novaStateRead(&st);
    CHECK(st.apArrivedMs == 1000);
    CHECK(st.apGen == 1);
    CHECK(st.lastMqttRxMs == 5000);

    // 同 id 的 approval_end 收口；异 id 不得关掉当前弹窗。
    const char* endSame = "{\"t\":\"approval_end\",\"id\":\"ap_7f3\"}";
    CHECK(protoApplyMessage("approval_end", endSame, std::strlen(endSame), 6000));
    novaStateRead(&st);
    CHECK(!st.approvalActive);
    CHECK(st.apGen == 2);

    // 已收口的旧审批重放（持久会话）不能复活成幽灵弹窗。
    CHECK(protoApplyMessage("approval", apJson, std::strlen(apJson), 6100));
    novaStateRead(&st);
    CHECK(!st.approvalActive);
    CHECK(st.apGen == 2);

    // 换一个新 id 才能重新弹窗。
    const char* ap3 = "{\"t\":\"approval\",\"id\":\"ap_third\",\"tool\":\"Bash\",\"ttl\":20000}";
    CHECK(protoApplyMessage("approval", ap3, std::strlen(ap3), 7000));
    const char* endOther = "{\"t\":\"approval_end\",\"id\":\"ap_other\"}";
    CHECK(protoApplyMessage("approval_end", endOther, std::strlen(endOther), 7100));
    novaStateRead(&st);
    CHECK(st.approvalActive);

    // ⚠️ 已知缺陷：空 id 的 approval_end 会无条件清场（proto.cpp:217）。
    const char* endEmpty = "{\"t\":\"approval_end\"}";
    CHECK(protoApplyMessage("approval_end", endEmpty, std::strlen(endEmpty), 7200));
    novaStateRead(&st);
    CHECK(!st.approvalActive);
    IDEAL_CHECK(st.approvalActive);

    // ⚠️ 已知缺陷 F6：设备只有一个 apId 槽，第二个审批直接覆盖第一个。
    const char* ap2 = "{\"t\":\"approval\",\"id\":\"ap_second\",\"tool\":\"Bash\",\"ttl\":20000}";
    novaStateInit("testdev", "0.5.0");
    CHECK(protoApplyMessage("approval", apJson, std::strlen(apJson), 8000));
    CHECK(protoApplyMessage("approval", ap2, std::strlen(ap2), 8100));
    novaStateRead(&st);
    CHECK(std::strcmp(st.apId, "ap_second") == 0);
    IDEAL_CHECK(std::strcmp(st.apId, "ap_7f3") == 0);

    // ttl 缺省口径：proto.cpp:206 是 8000，main.cpp / page_approval.cpp 却按 15000 兜底。
    const char* noTtl = "{\"t\":\"approval\",\"id\":\"ap_nottl\",\"tool\":\"Bash\"}";
    CHECK(protoApplyMessage("approval", noTtl, std::strlen(noTtl), 8200));
    novaStateRead(&st);
    CHECK(st.apTtlMs == 8000);

    // 超长中文 reason：必须按 UTF-8 边界截断，不能出现半个汉字。
    std::string longReason;
    for (int i = 0; i < 40; i++) longReason += "删除六个录音";
    char longJson[1024];
    std::snprintf(longJson, sizeof(longJson),
                  "{\"t\":\"approval\",\"id\":\"ap_long\",\"tool\":\"Bash\",\"reason\":\"%s\"}",
                  longReason.c_str());
    CHECK(protoApplyMessage("approval", longJson, std::strlen(longJson), 8300));
    novaStateRead(&st);
    CHECK(std::strlen(st.apReason) > 0);
    CHECK(std::strlen(st.apReason) < sizeof(st.apReason));
    CHECK(validUtf8(st.apReason));
    CHECK(std::strlen(st.apId) < sizeof(st.apId));

    // 上行决策三种组合，字段名必须与宿主 handleUp (bridge.js:556-567) 对齐。
    char buf[448] = {};
    size_t n = protoBuildDecision(buf, sizeof(buf), "ap_7f3", true, false);
    CHECK(n > 0 && n < sizeof(buf));
    CHECK(std::strstr(buf, "\"evt\":\"decision\"") != nullptr);
    CHECK(std::strstr(buf, "\"id\":\"ap_7f3\"") != nullptr);
    CHECK(std::strstr(buf, "\"accept\":true") != nullptr);
    CHECK(std::strstr(buf, "toPc") == nullptr);

    n = protoBuildDecision(buf, sizeof(buf), "ap_7f3", false, false);
    CHECK(n > 0 && std::strstr(buf, "\"accept\":false") != nullptr);

    n = protoBuildDecision(buf, sizeof(buf), "ap_7f3", false, true);
    CHECK(n > 0 && std::strstr(buf, "\"toPc\":true") != nullptr);
    CHECK(std::strstr(buf, "\"accept\"") == nullptr);

    // 缓冲区不足时整条不发，不能发出半截 JSON。
    CHECK(protoBuildDecision(buf, 8, "ap_7f3", true, false) == 0);
  }

  // ── 10. choreo 动作名 → NvaAnimKey：24 项逐一对齐 ────────────────
  // 契约见 docs/pet-expressiveness-plan.md §1：0..23 的顺序就是枚举顺序，
  // 名字拼错 / 宿主发旧名都会在这里退化成 0（idle），不能让设备播错动作。
  {
    static const char* const kExpect[24] = {
      "idle", "thinking", "working", "waiting", "success", "error", "click", "sleep", "quota",
      "click_happy", "click_shy", "click_angry", "click_giggle",
      "crab_walk", "happy_run", "coding", "archive",
      "stretch", "yawn", "wake_up", "hum", "eat_token", "bubble", "quota_worry",
    };
    NovaState cs{};
    for (int i = 0; i < 24; i++) {
      char json[192];
      std::snprintf(json, sizeof(json),
                    "{\"mood\":\"巡游\",\"steps\":[{\"x\":184,\"speed\":\"walk\","
                    "\"anim\":\"%s\",\"dur\":11000,\"say\":\"\"}]}",
                    kExpect[i]);
      novaStateInit("testdev", "0.5.0");
      CHECK(protoApplyMessage("choreo", json, std::strlen(json), 9000 + (uint32_t)i));
      novaStateRead(&cs);
      CHECK(cs.nChoreoSteps == 1);
      CHECK(cs.choreoSteps[0].anim == (uint8_t)i);
    }

    // 一条消息塞满 6 步（MAX_CHOREO_STEPS）时也要逐个对位，不能串号。
    const char* six =
        "{\"steps\":["
        "{\"anim\":\"stretch\"},{\"anim\":\"yawn\"},{\"anim\":\"wake_up\"},"
        "{\"anim\":\"hum\"},{\"anim\":\"eat_token\"},{\"anim\":\"bubble\"}]}";
    novaStateInit("testdev", "0.5.0");
    CHECK(protoApplyMessage("choreo", six, std::strlen(six), 9100));
    novaStateRead(&cs);
    CHECK(cs.nChoreoSteps == 6);
    for (int i = 0; i < 6; i++) CHECK(cs.choreoSteps[i].anim == (uint8_t)(17 + i));

    // 超过 6 步按上限截断：多出来的直接丢，且不越界写。
    char many[1024] = {};
    std::strcat(many, "{\"steps\":[");
    for (int i = 0; i < 24; i++) {
      if (i) std::strcat(many, ",");
      std::strcat(many, "{\"anim\":\"success\"}");
    }
    std::strcat(many, "]}");
    novaStateInit("testdev", "0.5.0");
    CHECK(protoApplyMessage("choreo", many, std::strlen(many), 9200));
    novaStateRead(&cs);
    CHECK(cs.nChoreoSteps == 6);

    // 未知名 / 大小写不符 / 空串 / 缺字段：一律回落 idle(0)，消息本身仍算应用。
    const char* unknown =
        "{\"steps\":[{\"anim\":\"quota_empty\"},{\"anim\":\"IDLE\"},"
        "{\"anim\":\"\"},{\"x\":120,\"dur\":12000}]}";
    novaStateInit("testdev", "0.5.0");
    CHECK(protoApplyMessage("choreo", unknown, std::strlen(unknown), 9300));
    novaStateRead(&cs);
    CHECK(cs.nChoreoSteps == 4);
    for (int i = 0; i < 4; i++) CHECK(cs.choreoSteps[i].anim == 0);

    // steps 为空必须原地拒绝：否则一条空剧本就能把设备上跑着的动作清掉。
    const char* empty = "{\"mood\":\"空\",\"steps\":[]}";
    CHECK(!protoApplyMessage("choreo", empty, std::strlen(empty), 9400));
  }

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
