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

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
  g_checks++; \
  if (!(cond)) { \
    g_fail++; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
  } \
} while (0)

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
  NovaSettings hc{};
  hc.soundPct = 255;
  n = protoBuildHello(buf, 320, hs, hc, true);
  CHECK(n > 0 && n < 320);
  CHECK(std::strlen(buf) == n);

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

  // ── 6. 录音回传确认载荷（mic_ack）────────────────────────────────
  {
    novaStateInit("testdev", "0.5.0");
    const char* okAck = "{\"id\":101,\"ok\":true,\"file\":\"rec_001_24dB_3.4s.wav\",\"missing\":0}";
    CHECK(protoApplyMessage("mic_ack", okAck, std::strlen(okAck), 3000));
    NovaState st;
    novaStateRead(&st);
    CHECK(st.recState == 4);
    CHECK(st.recSavedCount == 1);
    CHECK(std::strcmp(st.recLastFile, "rec_001_24dB_3.4s.wav") == 0);
    CHECK(st.recErr[0] == '\0');

    // Duplicate clipId does not double-count recSavedCount.
    CHECK(protoApplyMessage("mic_ack", okAck, std::strlen(okAck), 3100));
    novaStateRead(&st);
    CHECK(st.recSavedCount == 1);

    const char* badAck = "{\"id\":102,\"ok\":false,\"missing\":3}";
    CHECK(protoApplyMessage("mic_ack", badAck, std::strlen(badAck), 3200));
    novaStateRead(&st);
    CHECK(st.recState == 5);
    CHECK(std::strstr(st.recErr, "3") != nullptr);
  }

  // ── 7. 多轮对话协议（chat asr/reply + chat_send/chat_tts 构造）───
  {
    novaStateInit("testdev", "0.5.0");
    NovaState st;
    novaStateRead(&st);
    CHECK(st.recGainDb == 24);
    CHECK(st.nChatMsgs == 0);
    CHECK(!st.chatDraftActive);

    // 7a. ASR 成功
    const char* asrOk = "{\"t\":\"asr\",\"id\":42,\"ok\":true,\"text\":\"查一下当前状态\",\"ms\":610}";
    CHECK(protoApplyMessage("chat", asrOk, std::strlen(asrOk), 4000));
    novaStateRead(&st);
    CHECK(st.recState == 0);
    CHECK(st.chatDraftActive);
    CHECK(st.chatDraftId == 42);
    CHECK(st.chatDraftStartMs == 4000);
    CHECK(std::strcmp(st.chatDraftText, "查一下当前状态") == 0);
    CHECK(st.recErr[0] == '\0');
    CHECK(st.recGen == 1);
    CHECK(st.chatGen == 1);

    // 7b. ASR 空文本 / 失败
    const char* asrEmpty = "{\"t\":\"asr\",\"id\":43,\"ok\":true,\"text\":\"   \"}";
    CHECK(protoApplyMessage("chat", asrEmpty, std::strlen(asrEmpty), 4100));
    novaStateRead(&st);
    CHECK(st.recState == 5);
    CHECK(!st.chatDraftActive);
    CHECK(std::strcmp(st.recErr, "未识别到语音") == 0);

    const char* asrFail = "{\"t\":\"asr\",\"id\":44,\"ok\":false,\"err\":\"识别超时\"}";
    CHECK(protoApplyMessage("chat", asrFail, std::strlen(asrFail), 4200));
    novaStateRead(&st);
    CHECK(st.recState == 5);
    CHECK(std::strcmp(st.recErr, "识别超时") == 0);

    // 7c. Reply 成功 + 队列左移（超过 MAX_CHAT_MSGS = 8）
    novaStateWithLock([](NovaState& s) {
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
    const char* asrOk = "{\"t\":\"asr\",\"id\":7,\"ok\":true,\"text\":\"继续弄\"}";
    CHECK(protoApplyMessage("chat", asrOk, std::strlen(asrOk), 5100));
    novaStateRead(&st);
    CHECK(st.chatDraftActive);
    CHECK(std::strcmp(st.boardTargetId, "s3") == 0);

    // 若用户原本就指向 s1 (RUN)，ASR 后保持用户选择不覆盖
    novaStateWithLock([](NovaState& s) {
      std::strcpy(s.boardTargetId, "s1");
      s.chatDraftActive = false;
    });
    CHECK(protoApplyMessage("chat", asrOk, std::strlen(asrOk), 5200));
    novaStateRead(&st);
    CHECK(std::strcmp(st.boardTargetId, "s1") == 0);

    // 若用户原本指向 wait 会话 s2，ASR 后自动改锁可用会话
    novaStateWithLock([](NovaState& s) {
      std::strcpy(s.boardTargetId, "s2");
      s.chatDraftActive = false;
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

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
