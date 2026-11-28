#include "anim/nva.h"
#include "model/settings.h"
#include "model/state.h"
#include "ui/canvas.h"
#include "ui/nav.h"
#include "ui/theme.h"
#include <canvas/Arduino_Canvas.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

static void writeBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t)(v >> 24));
  out.push_back((uint8_t)(v >> 16));
  out.push_back((uint8_t)(v >> 8));
  out.push_back((uint8_t)v);
}

static void writeChunk(std::vector<uint8_t>& out, const char tag[4], const uint8_t* data, size_t len) {
  writeBe32(out, (uint32_t)len);
  size_t tagPos = out.size();
  out.insert(out.end(), tag, tag + 4);
  if (len && data) out.insert(out.end(), data, data + len);
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, out.data() + tagPos, (uInt)(4 + len));
  writeBe32(out, (uint32_t)crc);
}

static bool saveRgb565Png(const char* path, const uint16_t* fb, int w, int h) {
  std::vector<uint8_t> raw((size_t)h * (1 + w * 3));
  for (int y = 0; y < h; y++) {
    uint8_t* row = raw.data() + (size_t)y * (1 + w * 3);
    row[0] = 0;
    for (int x = 0; x < w; x++) {
      uint16_t c = fb[y * w + x];
      row[1 + x * 3 + 0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
      row[1 + x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
      row[1 + x * 3 + 2] = (uint8_t)((c & 0x1F) * 255 / 31);
    }
  }
  uLongf compLen = compressBound((uLong)raw.size());
  std::vector<uint8_t> comp(compLen);
  if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;

  std::vector<uint8_t> png = { 137, 80, 78, 71, 13, 10, 26, 10 };
  uint8_t ihdr[13] = {
    (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
    (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
    8, 2, 0, 0, 0
  };
  writeChunk(png, "IHDR", ihdr, sizeof(ihdr));
  writeChunk(png, "IDAT", comp.data(), (size_t)compLen);
  writeChunk(png, "IEND", nullptr, 0);

  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fwrite(png.data(), 1, png.size(), f);
  std::fclose(f);
  return true;
}

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

static void populateSample(NovaState& st) {
  std::memset(&st, 0, sizeof(st));
  std::strcpy(st.deviceId, "D93468");
  std::strcpy(st.fwVer, NOVA_FW_VER);
  std::strcpy(st.ip, "192.168.31.118");
  st.rssi = -52;
  st.wifiConnected = true;
  st.mqttConnected = true;
  st.run = 1;
  st.wait = 0;
  st.sess = 3;
  std::strcpy(st.msg, "Bash: ./tools/pio.sh run -e waveshare");
  st.ctxPct = 42;
  st.hour = 23; st.minute = 41; st.second = 5;
  st.month = 11; st.day = 22; st.dow = 0;   // 2026-11-22 是周日
  st.timeValid = true;
  st.drawFps = 55.0f;
  st.lastDrawMs = 1000;
  st.drawCount = 12345;
  st.effBright = 100;
  std::strcpy(st.activeModel, "GPT-6.2 Astra");

  st.nCards = 3;
  std::strcpy(st.cards[0].id, "ds");
  std::strcpy(st.cards[0].name, "DeepSeek");
  std::strcpy(st.cards[0].kind, "bal");
  st.cards[0].val = 77.10;
  std::strcpy(st.cards[0].cur, "CNY");
  st.cards[0].ageSec = 12;

  std::strcpy(st.cards[1].id, "oc");
  std::strcpy(st.cards[1].name, "OpenCode Go");
  std::strcpy(st.cards[1].kind, "win");
  st.cards[1].ageSec = 45;
  st.cards[1].nBars = 2;
  st.cards[1].bars[0] = { 73, "w", 7200 };
  st.cards[1].bars[1] = { 66, "m", 864000 };

  std::strcpy(st.cards[2].id, "od");
  std::strcpy(st.cards[2].name, "OpenDesign");
  std::strcpy(st.cards[2].kind, "win");
  st.cards[2].ageSec = 18;
  st.cards[2].nBars = 2;
  st.cards[2].bars[0] = { 0, "5h", 18000 };
  st.cards[2].bars[1] = { 39, "w", 302400 };

  std::strcpy(st.rankSrc, "ModelDial");
  std::strcpy(st.rankLic, "CC BY 4.0");
  st.nRows = 24;
  const char* models[8] = {
    "Claude Fable 5.2", "GPT-6.2 Astra", "Gemini 4 Pro", "DeepSeek V4",
    "Qwen 3.5 Max", "o4-mini High", "Sonnet 4.6", "GLM-5 Pro"
  };
  for (int i = 0; i < 24; i++) {
    st.rows[i].r = (uint8_t)(i + 1);
    if (i < 8) std::strcpy(st.rows[i].m, models[i]);
    else std::snprintf(st.rows[i].m, sizeof(st.rows[i].m), "Model-%02d", i + 1);
    std::strcpy(st.rows[i].e, (i == 0) ? "max" : "xhigh");
    st.rows[i].isNew = (i < 2);
    st.rows[i].s = 72.9f - i * 1.1f;
    st.rows[i].b = (uint8_t)(68 - i);
    st.rows[i].f = (uint8_t)(85 - i * 2);
    st.rows[i].k = (uint8_t)(100 - i * 2);
    st.rows[i].a = (uint8_t)(58 - i);
    st.rows[i].q = (uint8_t)(75 - i);
    std::snprintf(st.rows[i].dur, sizeof(st.rows[i].dur), "%dm", 120 + i * 15);
    std::snprintf(st.rows[i].cost, sizeof(st.rows[i].cost), "$%.1f", 45.0 - i * 1.2);
  }

  st.fps = 60.0f;
  st.cpuMhz = 240;
  st.nowMs = 10000;
  st.lastMqttRxMs = 8000;
  st.quotaRxMs = 5000;  // +5s live increment in quota view
  st.burnInStep = 0;
  st.pushUs = 8600;
  st.freeHeap = 182 * 1024;
  st.freePsram = 7400 * 1024;
  st.battMv = 4120;
  st.battPct = 92;
  st.usbPresent = true;
  st.charging = true;
  st.battStage = (uint8_t)BattStage::CC;
  st.battTempC = 31;
  st.chgMa = 300;
  st.chargeLimited = false;
  st.gaugePct = 91;
  st.hour = 16;
  st.minute = 55;
  st.second = 10;

  static constexpr const char* kTokenDates[14] = {
    "11/09", "11/10", "11/11", "11/12", "11/13", "11/14", "11/15",
    "11/16", "11/17", "11/18", "11/19", "11/20", "11/21", "11/22"
  };
  static constexpr uint64_t kTokenVals[14] = {
    169000000ULL, 107000000ULL, 137000000ULL, 139000000ULL, 25700000ULL,
    106000000ULL, 286000000ULL, 178000000ULL, 132000000ULL, 59000000ULL,
    73000000ULL,  238000000ULL, 287000000ULL, 30310000ULL
  };
  st.nTokenDays = 14;
  for (int i = 0; i < 14; i++) {
    std::strcpy(st.tokenDays[i].date, kTokenDates[i]);
    st.tokenDays[i].tokens = kTokenVals[i];
  }
  st.tokenTotal = 4040000000ULL;
  st.tokenAvgOverride = 141000000ULL;
  st.tokenSpanDays = 20;
  st.tokenCachePct = 96.6f;
}

static void mutateQuotaEmpty(NovaState& st, NovaSettings&) {
  st.nCards = 0;
}

static void mutateQuotaWarn(NovaState& st, NovaSettings&) {
  st.cards[0].val = 3.20;
  st.cards[1].bars[0].pct = 92;
  st.cards[1].bars[1].pct = 78;
}

// 空态用例必须喂宿主真实下发的原因码（fetchers.js 只发 no-session / no-key /
// reauth / fetch-err，kind 只会是 "na"）。以前这里是 "none" + 中文 "缺少 Cookie"，
// 走的是 whyText() 的未知兜底分支，真实路径一条都没覆盖到。
static void mutateQuotaUnconf(NovaState& st, NovaSettings&) {
  std::snprintf(st.cards[2].kind, sizeof(st.cards[2].kind), "na");
  std::snprintf(st.cards[2].why, sizeof(st.cards[2].why), "no-session");
}

static void mutateQuotaReauth(NovaState& st, NovaSettings&) {
  std::snprintf(st.cards[2].kind, sizeof(st.cards[2].kind), "na");
  std::snprintf(st.cards[2].why, sizeof(st.cards[2].why), "reauth");
}

static void mutateQuotaFetchErr(NovaState& st, NovaSettings&) {
  std::snprintf(st.cards[2].kind, sizeof(st.cards[2].kind), "na");
  std::snprintf(st.cards[2].why, sizeof(st.cards[2].why), "fetch-err");
}

static void mutateRankScrolled(NovaState& st, NovaSettings&) {
  const char* longModel = "Claude Fable 5.2 Ultra Long Context Preview";
  std::snprintf(st.rows[0].m, sizeof(st.rows[0].m), "%s", longModel);
  // Scroll down before draw so the simulator exercises clamped offset & scrollbar rendering.
  pageRankScrollPx(-600, 24);
}

static void cleanupRankScroll() {
  pageRankResetScroll();
}

static void mutateSettingsMuted(NovaState&, NovaSettings& cfg) {
  cfg.soundPct = 0;
  cfg.brightPct = 20;
  cfg.ttsEnabled = false;
  cfg.quietHours = false;
  cfg.choreoEnabled = false;
}

// 主屏码头扩展：会话名 + 工具名 + 异常详情浮层。
static void mutateDockDetail(NovaState& st, NovaSettings&) {
  std::snprintf(st.sessTitle, sizeof(st.sessTitle), "issue 核查与复盘");
  std::snprintf(st.lastTool, sizeof(st.lastTool), "bash");
}

static void mutateDockError(NovaState& st, NovaSettings& cfg) {
  mutateDockDetail(st, cfg);
  st.run = 0;
  st.turnError = true;
  std::snprintf(st.errMsg, sizeof(st.errMsg),
                "HTTP 429: rate limited (requestId=req_9f3a2c, status=429). "
                "Provider returned a long message that needs wrapping to show fully.");
  std::snprintf(st.msg, sizeof(st.msg), "本轮异常结束 · HTTP 429");
}

static void mutateDockErrorExpanded(NovaState& st, NovaSettings& cfg) {
  mutateDockError(st, cfg);
  st.dockExpanded = true;
}

// AP 配网页（wifiApMode && !wifiConnected）：决定 #3 加二维码前后的基准图。
// burnStep 用来复现 issue #3 §5 说的烧屏 bug —— AP 卡片写死 24,80，不跟偏移。
static void mutateApPortal(NovaState& st, NovaSettings&) {
  st.wifiConnected = false;
  st.wifiApMode = true;
  st.mqttConnected = false;
  std::snprintf(st.apSsid, sizeof(st.apSsid), "Nova-3468");
  st.burnInStep = 0;
}

static void mutateApPortalBurn(NovaState& st, NovaSettings& cfg) {
  mutateApPortal(st, cfg);
  st.burnInStep = 7;  // burnOffsetX=+2, burnOffsetY=+1
}

// 电源状态用例：顶栏三态 + 详情页状态串 + 设置页电源 tab。
// 潮汐出图时把 cfg.uiTheme 同步成 1，否则设置页「界面风格」会显示深海原版，
// 与实际渲染的主题对不上（真机上 main 已保证两者一致）。
static void mutateTideSettings(NovaState&, NovaSettings& cfg) {
  cfg.uiTheme = 1;
  cfg.brightPct = 75;
}

static void mutateTideHomeClean(NovaState& st, NovaSettings&) {
  st.nowMs = 20000;  // 避开前面局部测试残留的 13.5s 旧气泡
}

static void mutateTideHomeIdle(NovaState& st, NovaSettings&) {
  st.run = 0;
  st.wait = 0;
  st.activeModel[0] = '\0';
  st.msg[0] = '\0';
  st.nowMs = 20000;
}

static void mutateTideHomeAlert(NovaState& st, NovaSettings& cfg) {
  mutateDockError(st, cfg);
  st.nowMs = 20000;
}

// 潮汐 heart-beat 断线态：WiFi / MQTT 全断，页头切信号绯红。
static void mutateDetailOffline(NovaState& st, NovaSettings&) {
  st.wifiConnected = false;
  st.mqttConnected = false;
  st.lastMqttRxMs = 0;
}

static void mutateDetailCharging(NovaState& st, NovaSettings&) {
  st.battPct = 64;
  st.battMv = 4050;
  st.usbPresent = true;
  st.charging = true;
  st.battStage = (uint8_t)BattStage::CC;
  st.chgMa = 300;
  st.battTempC = 33;
  st.gaugePct = 61;
}

static void mutateDetailFull(NovaState& st, NovaSettings&) {
  st.battPct = 100;
  st.battMv = 4190;
  st.usbPresent = true;
  st.charging = false;
  st.battStage = (uint8_t)BattStage::DONE;
  st.chgMa = 0;
  st.battTempC = 30;
  st.gaugePct = 100;
}

static void mutateDetailLimited(NovaState& st, NovaSettings&) {
  st.battPct = 90;
  st.battMv = 4100;
  st.usbPresent = true;
  st.charging = false;
  st.battStage = (uint8_t)BattStage::LIMITED;
  st.chgMa = 0;
  st.chargeLimited = true;
  st.battTempC = 34;
  st.gaugePct = 88;
}

static void mutateDetailLow(NovaState& st, NovaSettings&) {
  st.battPct = 9;
  st.battMv = 3550;
  st.usbPresent = false;
  st.charging = false;
  st.battStage = (uint8_t)BattStage::DISCHARGE;
  st.chgMa = 0;
  st.battTempC = 29;
  st.gaugePct = 7;
}

static void mutateHomeCharging(NovaState& st, NovaSettings&) {
  st.battPct = 88;
  st.battMv = 4180;
  st.usbPresent = true;
  st.charging = true;
  st.battStage = (uint8_t)BattStage::CV;
  st.chgMa = 300;
  st.battTempC = 33;
  st.gaugePct = 86;
}

static void mutateSettingsPower(NovaState& st, NovaSettings& cfg) {
  cfg.chargeLimitPct = 90;
  cfg.chargeSpeed = ChargeSpeed::AUTO;
  mutateDetailCharging(st, cfg);
}

static void mutateApprovalUrgent(NovaState& st, NovaSettings&) {  st.apTtlMs = 20000;
  st.nowMs = 25000;
  st.apArrivedMs = st.nowMs - 18000;
  std::snprintf(
      st.apReason, sizeof(st.apReason),
      "cd /Volumes/hamelin-ssd/workspace/nova-buddy && git status && "
      "./tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8 -t upload && "
      "./tools/sim/run.sh /tmp/out");
}

static void mutateTokensSelected(NovaState& st, NovaSettings&) {
  // Tap the peak whale (index 12, "11/21") to exercise whale selection & exact token readout.
  pageTokensTap(315, 228, st, 6500);
}

static void cleanupTokensSelect() {
  pageTokensOnEnter(0);
}

static void mutateTokensEmpty(NovaState& st, NovaSettings&) {
  st.nTokenDays = 0;
}

static void mutateAodTime(NovaState& st, NovaSettings&) {
  st.hour = 23; st.minute = 41; st.second = 0;
}

static void mutateAodIdle(NovaState& st, NovaSettings& cfg) {
  mutateAodTime(st, cfg);
  st.run = 0;
  st.wait = 0;
}

static void mutateAodNoTime(NovaState& st, NovaSettings& cfg) {
  mutateAodTime(st, cfg);
  st.timeValid = false;
}

static void seedBoardSessions(NovaState& st) {
  st.recState = 0;
  st.recGainDb = 24;
  st.boardRxMs = st.nowMs;
  st.nBoardSessions = 3;

  std::memset(st.boardSessions, 0, sizeof(st.boardSessions));
  std::strcpy(st.boardSessions[0].id, "s1");
  std::strcpy(st.boardSessions[0].name, "电量估算四改");
  std::strcpy(st.boardSessions[0].cwd, "nova-buddy");
  st.boardSessions[0].state = BoardSessState::RUN;
  st.boardSessions[0].elapsedSec = 252;
  std::strcpy(st.boardSessions[0].act, "cargo test");
  std::strcpy(st.boardSessions[0].note, "2 个断言失败");
  st.boardSessions[0].nSteps = 3;
  std::strcpy(st.boardSessions[0].steps[0].time, "4:02");
  std::strcpy(st.boardSessions[0].steps[0].tool, "bash");
  std::strcpy(st.boardSessions[0].steps[0].arg, "cargo test --release");
  std::strcpy(st.boardSessions[0].steps[1].time, "3:40");
  std::strcpy(st.boardSessions[0].steps[1].tool, "edit");
  std::strcpy(st.boardSessions[0].steps[1].arg, "battery_model.h");
  std::strcpy(st.boardSessions[0].steps[2].time, "2:10");
  std::strcpy(st.boardSessions[0].steps[2].tool, "read");
  std::strcpy(st.boardSessions[0].steps[2].arg, "power_probe.mjs");

  std::strcpy(st.boardSessions[1].id, "s2");
  std::strcpy(st.boardSessions[1].name, "ASR 会话桥接");
  std::strcpy(st.boardSessions[1].cwd, "nova-buddy");
  st.boardSessions[1].state = BoardSessState::WAIT;
  std::strcpy(st.boardSessions[1].act, "bash · rm -rf ./recordings");
  std::strcpy(st.boardSessions[1].note, "会删 6 个录音");

  std::strcpy(st.boardSessions[2].id, "s3");
  std::strcpy(st.boardSessions[2].name, "DS-Bot 调研");
  std::strcpy(st.boardSessions[2].cwd, "ds-bot");
  st.boardSessions[2].state = BoardSessState::IDLE;
  std::strcpy(st.boardSessions[2].act, "已停下");
  std::strcpy(st.boardSessions[2].note, "8 分钟前停下");

  std::strcpy(st.lastSayText, "单测还剩 2 个失败，都在 fixtures 的时区断言上。");
  st.lastSayAgeSec = 240;
  st.lastSayRxMs = st.nowMs;
}

static void mutateChatThread(NovaState& st, NovaSettings&) {
  seedBoardSessions(st);
  st.boardGen = 1;
}

static void mutateChatSelected(NovaState& st, NovaSettings&) {
  seedBoardSessions(st);
  std::strcpy(st.boardTargetId, "s1");
  std::strcpy(st.boardExpandedId, "s1");
  st.boardGen = 2;
}

static void mutateChatRec(NovaState& st, NovaSettings& cfg) {
  mutateChatThread(st, cfg);
  st.recState = 1;
  st.recGainDb = 24;
  st.recLevel = 72;
  st.recDurMs = 4200;
  st.boardHoldCancel = false;
  st.chatGen = 2;
}

static void mutateChatRecCancel(NovaState& st, NovaSettings& cfg) {
  mutateChatRec(st, cfg);
  st.boardHoldCancel = true;
}

static void mutateChatDraft(NovaState& st, NovaSettings& cfg) {
  mutateChatThread(st, cfg);
  st.recState = 0;
  std::strcpy(st.boardTargetId, "s3");
  st.chatDraftActive = true;
  st.chatDraftId = 13;
  st.chatDraftStartMs = st.nowMs - 550u;
  std::snprintf(st.chatDraftText, sizeof(st.chatDraftText), "总结一下刚才查到的认证流程");
  st.chatGen = 3;
}

static void mutateChatDraftRun(NovaState& st, NovaSettings& cfg) {
  mutateChatThread(st, cfg);
  st.recState = 0;
  std::strcpy(st.boardTargetId, "s1");
  st.chatDraftActive = true;
  st.chatDraftId = 14;
  st.chatDraftStartMs = st.nowMs - 550u;
  std::snprintf(st.chatDraftText, sizeof(st.chatDraftText), "接着弄电量那个，把测试跑完");
  st.chatGen = 4;
}

static void mutateChatRun(NovaState& st, NovaSettings& cfg) {
  mutateChatThread(st, cfg);
  std::strcpy(st.boardTargetId, "s1");
  st.chatDelivery.kind = ChatDelivKind::RUN;
  st.chatDelivery.id = 14;
  st.chatDelivery.secs = 4;
  st.chatDelivery.updatedMs = st.nowMs;
  std::strcpy(st.chatDelivery.targetId, "s1");
  std::strcpy(st.chatDelivery.targetName, "电量估算四改");
}

static void mutateChatReply(NovaState& st, NovaSettings& cfg) {
  mutateChatThread(st, cfg);
  std::strcpy(st.boardTargetId, "s1");
  st.chatDelivery.kind = ChatDelivKind::REPLY;
  st.chatDelivery.id = 14;
  st.chatDelivery.secs = 4;
  st.chatDelivery.updatedMs = st.nowMs;
  std::strcpy(st.chatDelivery.targetId, "s1");
  std::strcpy(st.chatDelivery.targetName, "电量估算四改");
  std::strcpy(st.chatDelivery.text, "单测还剩 2 个失败，都在 fixtures 的时区断言上。");
}

static void mutateChatFail(NovaState& st, NovaSettings& cfg) {
  mutateChatThread(st, cfg);
  std::strcpy(st.boardTargetId, "s1");
  st.chatDelivery.kind = ChatDelivKind::FAIL;
  st.chatDelivery.id = 15;
  st.chatDelivery.updatedMs = st.nowMs;
  std::strcpy(st.chatDelivery.targetId, "s1");
  std::strcpy(st.chatDelivery.text, "目标会话已换段，需要重选");
}

static void mutateChatMute(NovaState& st, NovaSettings& cfg) {
  mutateChatThread(st, cfg);
  std::strcpy(st.boardTargetId, "s1");
  st.chatDelivery.kind = ChatDelivKind::MUTE;
  st.chatDelivery.id = 16;
  st.chatDelivery.updatedMs = st.nowMs;
  std::strcpy(st.chatDelivery.targetId, "s1");
}

static void mutateHomeCoding(NovaState& st, NovaSettings&) {
  st.run = 1;
  st.wait = 0;
  std::snprintf(st.lastTool, sizeof(st.lastTool), "edit");
  std::snprintf(st.sessTitle, sizeof(st.sessTitle), "表现力状态机");
  std::snprintf(st.msg, sizeof(st.msg), "edit: src/ui/page_home.cpp");
}

static void mutateHomeYawn(NovaState& st, NovaSettings&) {
  st.run = 0;
  st.wait = 0;
  st.activeModel[0] = '\0';
  st.msg[0] = '\0';
}

static void mutateHomeSleepZzz(NovaState& st, NovaSettings&) {
  st.run = 0;
  st.wait = 0;
  st.activeModel[0] = '\0';
  st.msg[0] = '\0';
}

int main(int argc, char** argv) {
  std::string outDir = (argc > 1) ? argv[1] : "tools/sim/out";
  Arduino_Canvas canvas(UI_W, UI_H, nullptr);
  if (!canvas.begin(GFX_SKIP_OUTPUT_BEGIN)) return 1;
  canvas.setUTF8Print(true);
  nvaInit("assets/pets/nova");

  FILE* manifest = std::fopen((outDir + "/manifest.txt").c_str(), "w");
  if (!manifest) {
    std::printf("cannot write %s/manifest.txt\n", outDir.c_str());
    return 1;
  }

  UiNav nav;

  struct Case {
    const char* name;
    UiPage page;
    bool approval;
    uint8_t settingsTab;
    uint32_t drawNowMs;
    void (*mutate)(NovaState&, NovaSettings&);
    void (*afterDraw)();
    bool aod = false;
    uint8_t openId = 0;     // 设置页：展开哪一行（RowId，0 = 不展开）
    uint8_t confirmId = 0;  // 设置页：哪一行进二次确认
    int8_t anim = -1;       // 非 AOD 首页展示指定 NvaAnimKey
    uint8_t animTicks = 0;
  };
  Case cases[] = {
    { "home", UiPage::HOME, false, 0, 6500, nullptr, nullptr },
    { "quota", UiPage::QUOTA, false, 0, 6500, nullptr, nullptr },
    { "rank", UiPage::RANK, false, 0, 6500, nullptr, nullptr },
    { "detail", UiPage::DETAIL, false, 0, 6500, nullptr, nullptr },
    { "settings", UiPage::SETTINGS, false, 0, 6500, nullptr, nullptr },
    { "settings_aod", UiPage::SETTINGS, false, 1, 6500, nullptr, nullptr },
    { "settings_sound", UiPage::SETTINGS, false, 2, 6500, nullptr, nullptr },
    { "settings_open", UiPage::SETTINGS, false, 0, 6500, nullptr, nullptr, false, 1 },   // 1 = R_BRIGHT
    { "settings_opts", UiPage::SETTINGS, false, 0, 6500, nullptr, nullptr, false, 2 },   // 2 = R_SLEEP
    { "settings_confirm", UiPage::SETTINGS, false, 0, 6500, nullptr, nullptr, false, 0, 22 },  // 22 = R_RESET
    { "approval", UiPage::HOME, true, 0, 6500, nullptr, nullptr },
    { "home_dock", UiPage::HOME, false, 0, 6500, mutateDockDetail, nullptr },
    { "home_dock_err", UiPage::HOME, false, 0, 6500, mutateDockError, nullptr },
    { "home_dock_detail", UiPage::HOME, false, 0, 6500, mutateDockErrorExpanded, nullptr },
    { "home_ap", UiPage::HOME, false, 0, 6500, mutateApPortal, nullptr },
    { "home_ap_burn", UiPage::HOME, false, 0, 6500, mutateApPortalBurn, nullptr },
    { "quota_empty", UiPage::QUOTA, false, 0, 6500, mutateQuotaEmpty, nullptr },
    { "quota_warn", UiPage::QUOTA, false, 0, 6500, mutateQuotaWarn, nullptr },
    { "quota_unconf", UiPage::QUOTA, false, 0, 6500, mutateQuotaUnconf, nullptr },
    { "quota_reauth", UiPage::QUOTA, false, 0, 6500, mutateQuotaReauth, nullptr },
    { "quota_fetcherr", UiPage::QUOTA, false, 0, 6500, mutateQuotaFetchErr, nullptr },
    { "rank_scrolled", UiPage::RANK, false, 0, 6500, mutateRankScrolled, cleanupRankScroll },
    { "settings_muted", UiPage::SETTINGS, false, 0, 6500, mutateSettingsMuted, nullptr },
    { "home_charging", UiPage::HOME, false, 0, 6500, mutateHomeCharging, nullptr },
    { "detail_charging", UiPage::DETAIL, false, 0, 6500, mutateDetailCharging, nullptr },
    { "detail_full", UiPage::DETAIL, false, 0, 6500, mutateDetailFull, nullptr },
    { "detail_limited", UiPage::DETAIL, false, 0, 6500, mutateDetailLimited, nullptr },
    { "detail_low", UiPage::DETAIL, false, 0, 6500, mutateDetailLow, nullptr },
    { "settings_batt", UiPage::SETTINGS, false, 3, 6500, mutateSettingsPower, nullptr },
    { "approval_urgent", UiPage::HOME, true, 0, 25000, mutateApprovalUrgent, nullptr },
    { "tokens", UiPage::TOKENS, false, 0, 6500, nullptr, nullptr },
    { "tokens_selected", UiPage::TOKENS, false, 0, 6500, mutateTokensSelected, cleanupTokensSelect },
    { "tokens_empty", UiPage::TOKENS, false, 0, 6500, mutateTokensEmpty, nullptr },
    { "aod", UiPage::HOME, false, 0, 6500, mutateAodTime, nullptr, true },
    { "aod_idle", UiPage::HOME, false, 0, 6500, mutateAodIdle, nullptr, true },
    { "aod_nolock", UiPage::HOME, false, 0, 6500, mutateAodNoTime, nullptr, true },
    { "chat", UiPage::CHAT, false, 0, 6500, mutateChatThread, nullptr },
    { "chat_selected", UiPage::CHAT, false, 0, 6500, mutateChatSelected, nullptr },
    { "chat_rec", UiPage::CHAT, false, 0, 6500, mutateChatRec, nullptr },
    { "chat_rec_cancel", UiPage::CHAT, false, 0, 6500, mutateChatRecCancel, nullptr },
    { "chat_draft", UiPage::CHAT, false, 0, 6500, mutateChatDraft, nullptr },
    { "chat_draft_run", UiPage::CHAT, false, 0, 6500, mutateChatDraftRun, nullptr },
    { "chat_run", UiPage::CHAT, false, 0, 6500, mutateChatRun, nullptr },
    { "chat_reply", UiPage::CHAT, false, 0, 6500, mutateChatReply, nullptr },
    { "chat_fail", UiPage::CHAT, false, 0, 6500, mutateChatFail, nullptr },
    { "chat_mute", UiPage::CHAT, false, 0, 6500, mutateChatMute, nullptr },
    { "home_coding", UiPage::HOME, false, 0, 6500, mutateHomeCoding, nullptr, false, 0, 0, (int8_t)NvaAnimKey::CODING, 16 },
    { "home_yawn", UiPage::HOME, false, 0, 6500, mutateHomeYawn, nullptr, false, 0, 0, (int8_t)NvaAnimKey::YAWN, 22 },
    { "home_sleep_zzz", UiPage::HOME, false, 0, 6500, mutateHomeSleepZzz, nullptr, false, 0, 0, (int8_t)NvaAnimKey::SLEEP, 18 },
  };

  for (const auto& cs : cases) {
    // Isolate each case on fresh sample state/settings so mutations never leak across cases.
    NovaState st;
    populateSample(st);    NovaSettings cfg;
    st.approvalActive = cs.approval;
    std::strcpy(st.apId, "ap_7f3");
    std::strcpy(st.apTool, "Bash");
    std::strcpy(st.apReason, "rm -rf /tmp/nova-cache && ./tools/pio.sh run -t uploadfs");
    st.apTtlMs = 8000;
    st.apArrivedMs = 1000;
    if (cs.mutate) cs.mutate(st, cfg);
    pageSettingsSetTab(cs.settingsTab);
    if (cs.openId)    pageSettingsDebugOpen(cs.openId);
    if (cs.confirmId) pageSettingsDebugConfirm(cs.confirmId);
    nav.setPage(cs.page);
    if (cs.aod) {
      // 真机 AOD 统一以睡觉姿势缓慢循环呼吸；模拟器同样先播三帧睡觉姿势。
      nvaSetState(NvaAnimKey::SLEEP, false);
      for (int i = 0; i < 3; i++) nvaTick(cs.drawNowMs + (uint32_t)i * 120u);
      pageHomeDrawAod(canvas, st, cs.drawNowMs);
    } else {
      if (cs.anim >= 0) {
        nvaSetState((NvaAnimKey)cs.anim, false, true);
        for (uint8_t i = 0; i < cs.animTicks; i++) {
          nvaTick(cs.drawNowMs + (uint32_t)i * 120u);
        }
      }
      nav.draw(canvas, st, cfg, 24, cs.drawNowMs);
    }
    if (cs.afterDraw) cs.afterDraw();
    std::string path = outDir + "/page_" + cs.name + ".png";
    if (!saveRgb565Png(path.c_str(), canvas.getFramebuffer(), UI_W, UI_H)) return 1;
    const uint32_t h = fnv1a32(reinterpret_cast<const uint8_t*>(canvas.getFramebuffer()),
                               (size_t)UI_W * UI_H * sizeof(uint16_t));
    std::fprintf(manifest, "page_%s %08x\n", cs.name, h);
    std::printf("wrote %s\n", path.c_str());
  }

  // page_tokens_local：局部重绘契约 —— pageTokensDrawStage() 不许碰
  // pageTokensStageRect() 之外的像素。做法：整屏填哨兵，再局部重绘；
  // rect 外必须还是哨兵，rect 内必须真的被写过。哈希行固定为 "page_tokens_local ok"。
  {
    NovaState st;
    populateSample(st);
    NovaSettings cfg;
    nav.setPage(UiPage::TOKENS);
    pageTokensOnEnter(6500);

    constexpr uint16_t kSentinel = 0xA55A;
    uint16_t* fb = canvas.getFramebuffer();
    for (size_t i = 0; i < (size_t)UI_W * UI_H; i++) fb[i] = kSentinel;

    pageTokensDrawStage(canvas, st, cfg, 7000);

    const ui::Rect r = pageTokensStageRect();
    size_t outside = 0, inside = 0;
    int firstX = -1, firstY = -1;
    for (int y = 0; y < UI_H; y++) {
      for (int x = 0; x < UI_W; x++) {
        const bool in = (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h);
        const size_t idx = (size_t)y * UI_W + x;
        if (in) {
          if (fb[idx] != kSentinel) inside++;
        } else if (fb[idx] != kSentinel) {
          if (outside == 0) { firstX = x; firstY = y; }
          outside++;
        }
      }
    }
    if (outside != 0) {
      std::printf("page_tokens_local: %zu pixels written outside stage rect "
                  "(first at %d,%d; rect=%d,%d %dx%d)\n",
                  outside, firstX, firstY, r.x, r.y, r.w, r.h);
      std::fclose(manifest);
      return 1;
    }
    if (inside == 0) {
      std::printf("page_tokens_local: stage drew nothing (rect=%d,%d %dx%d)\n",
                  r.x, r.y, r.w, r.h);
      std::fclose(manifest);
      return 1;
    }
    std::fprintf(manifest, "page_tokens_local ok\n");
  }

  // page_home_stage_local：主屏舞台局部重绘契约 —— Zzz / 火花 / 气泡 / 防烧屏偏移
  // 必须严格落在返回的脏矩形内，且脏矩形不许越过舞台边界 [0..UI_W, 62..304]。
  {
    NovaState st;
    populateSample(st);
    st.run = 0;
    st.wait = 0;
    st.activeModel[0] = '\0';
    st.msg[0] = '\0';
    NovaSettings cfg;
    nav.setPage(UiPage::HOME);
    nvaSetState(NvaAnimKey::SLEEP, false, true);
    for (int i = 0; i < 6; i++) nvaTick(6500u + (uint32_t)i * 120u);
    nav.draw(canvas, st, cfg, 24, 6500);

    // 触发火花与气泡后再切回 SLEEP，同时在最不利防烧屏偏移下验证两帧局部重绘。
    pageHomeTap(184, 220, st);
    nvaSetState(NvaAnimKey::SLEEP, false, true);
    for (int i = 0; i < 4; i++) nvaTick(7300u + (uint32_t)i * 120u);

    constexpr uint16_t kSentinel = 0xA55A;
    uint16_t* fb = canvas.getFramebuffer();
    const uint8_t burnSteps[2] = { 0, 7 };
    for (int step = 0; step < 2; step++) {
      st.burnInStep = burnSteps[step];
      st.nowMs = 10100u + (uint32_t)step * 100u;
      for (size_t i = 0; i < (size_t)UI_W * UI_H; i++) fb[i] = kSentinel;

      int16_t dx = 0, dy = 0, dw = 0, dh = 0;
      pageHomeDrawStage(canvas, st, cfg, (uint32_t)(25 + step), &dx, &dy, &dw, &dh);
      if (dw <= 0 || dh <= 0 || dx < 0 || dy < 62 || dx + dw > UI_W || dy + dh > 304) {
        std::printf("page_home_stage_local: dirty rect out of stage bounds (%d,%d %dx%d)\n",
                    dx, dy, dw, dh);
        std::fclose(manifest);
        return 1;
      }
      size_t outside = 0, inside = 0;
      int firstX = -1, firstY = -1;
      for (int y = 0; y < UI_H; y++) {
        for (int x = 0; x < UI_W; x++) {
          const bool in = (x >= dx && x < dx + dw && y >= dy && y < dy + dh);
          const size_t idx = (size_t)y * UI_W + x;
          if (in) {
            if (fb[idx] != kSentinel) inside++;
          } else if (fb[idx] != kSentinel) {
            if (outside == 0) { firstX = x; firstY = y; }
            outside++;
          }
        }
      }
      if (outside != 0) {
        std::printf("page_home_stage_local: %zu pixels written outside rect at step %d "
                    "(first at %d,%d; rect=%d,%d %dx%d)\n",
                    outside, step, firstX, firstY, dx, dy, dw, dh);
        std::fclose(manifest);
        return 1;
      }
      if (inside == 0) {
        std::printf("page_home_stage_local: stage drew nothing at step %d\n", step);
        std::fclose(manifest);
        return 1;
      }
    }
    std::fprintf(manifest, "page_home_stage_local ok\n");
  }

  // ── 潮汐（TIDE）主题出图：15 张独立 PNG，只写文件、不写 manifest ─────────
  // 基线（51 项）必须零变更，所以这里刻意不 fprintf 任何哈希行；
  // check.sh 只比对 manifest，这些图仅供人工 / Carbon 逐张终审。
  {
    struct TideCase {
      const char* name;
      UiPage page;
      bool approval;
      uint8_t settingsTab;
      uint32_t drawNowMs;
      void (*mutate)(NovaState&, NovaSettings&);
      void (*afterDraw)();
      bool aod;
      uint8_t openId;
      uint8_t confirmId;
      int8_t anim;
      uint8_t animTicks;
    };
    TideCase tides[] = {
      { "home_work",      UiPage::HOME,     false, 0, 20000, mutateTideHomeClean, nullptr, false, 0, 0, (int8_t)NvaAnimKey::CODING, 3 },
      { "home_idle",      UiPage::HOME,     false, 0, 20000, mutateTideHomeIdle,  nullptr, false, 0, 0, (int8_t)NvaAnimKey::IDLE,   2 },
      { "home_alert",     UiPage::HOME,     false, 0, 20000, mutateTideHomeAlert, nullptr, false, 0, 0, (int8_t)NvaAnimKey::ERROR,  3 },
      { "tokens",         UiPage::TOKENS,   false, 0, 6500, nullptr,              nullptr, false, 0, 0, -1 },
      { "quota",          UiPage::QUOTA,    false, 0, 6500, nullptr,              nullptr, false, 0, 0, -1 },
      { "approval",       UiPage::HOME,     true,  0, 6500, nullptr,              nullptr, false, 0, 0, -1 },
      { "chat_list",      UiPage::CHAT,     false, 0, 6500, mutateChatThread,     nullptr, false, 0, 0, -1 },
      { "chat_expand",    UiPage::CHAT,     false, 0, 6500, mutateChatSelected,   nullptr, false, 0, 0, -1 },
      { "chat_rec",       UiPage::CHAT,     false, 0, 6500, mutateChatRec,        nullptr, false, 0, 0, -1 },
      { "chat_draft",     UiPage::CHAT,     false, 0, 6500, mutateChatDraft,      nullptr, false, 0, 0, -1 },
      // 展开「亮度」行：一张图里同时给出「界面风格」行与 38px 液位滑块。
      { "settings",       UiPage::SETTINGS, false, 0, 6500, mutateTideSettings,   nullptr, false, 1, 0, -1 },
      { "rank",           UiPage::RANK,     false, 0, 6500, nullptr,              nullptr, false, 0, 0, -1 },
      { "detail_online",  UiPage::DETAIL,   false, 0, 6500, nullptr,              nullptr, false, 0, 0, -1 },
      { "detail_offline", UiPage::DETAIL,   false, 0, 6500, mutateDetailOffline,  nullptr, false, 0, 0, -1 },
      { "aod",            UiPage::HOME,     false, 0, 6500, mutateAodTime,        nullptr, true,  0, 0, -1 },
    };

    const ui::UiTheme savedTheme = ui::activeTheme();
    ui::setActiveTheme(ui::UiTheme::TIDE);

    for (const auto& tc : tides) {
      NovaState st;
      populateSample(st);
      NovaSettings cfg;
      st.approvalActive = tc.approval;
      std::strcpy(st.apId, "ap_7f3");
      std::strcpy(st.apTool, "Bash");
      std::strcpy(st.apReason, "rm -rf /tmp/nova-cache && ./tools/pio.sh run -t uploadfs");
      st.apTtlMs = 8000;
      st.apArrivedMs = 1000;
      if (tc.mutate) tc.mutate(st, cfg);
      cfg.uiTheme = 1;
      pageSettingsSetTab(tc.settingsTab);
      if (tc.openId)    pageSettingsDebugOpen(tc.openId);
      if (tc.confirmId) pageSettingsDebugConfirm(tc.confirmId);
      // 前面 page_tokens_local 把 s_enterMs 设成 6500，鲸鱼群会以为刚入场而全员 alpha=0。
      // 这里回到 0，让 6500ms 时游动动画已经结束、全部落位（与基线 tokens 用例同帧）。
      if (tc.page == UiPage::TOKENS) pageTokensOnEnter(0);
      nav.setPage(tc.page);
      if (tc.aod) {
        // AOD 走原版 pageHomeDrawAod()：潮汐开关不该污染熄屏表盘（纯黑省电防烧屏）。
        const NvaAnimKey pose = (st.run > 0) ? NvaAnimKey::WORKING
                              : (st.wait > 0 ? NvaAnimKey::WAITING : NvaAnimKey::SLEEP);
        nvaSetState(pose, false);
        for (int i = 0; i < 3; i++) nvaTick(tc.drawNowMs + (uint32_t)i * 120u);
        pageHomeDrawAod(canvas, st, tc.drawNowMs);
      } else {
        if (tc.anim >= 0) {
          nvaSetState((NvaAnimKey)tc.anim, false, true);
          for (uint8_t i = 0; i < tc.animTicks; i++) nvaTick(tc.drawNowMs + (uint32_t)i * 120u);
        }
        nav.draw(canvas, st, cfg, 24, tc.drawNowMs);
      }
      if (tc.afterDraw) tc.afterDraw();
      std::string path = outDir + "/tide_" + tc.name + ".png";
      if (!saveRgb565Png(path.c_str(), canvas.getFramebuffer(), UI_W, UI_H)) return 1;
      std::printf("wrote %s\n", path.c_str());
    }

    ui::setActiveTheme(savedTheme);
    nav.setPage(UiPage::HOME);
    pageSettingsOnEnter();
    pageRankResetScroll();
    pageTokensOnEnter(0);
  }

  // tide_home_stage_local：潮汐主屏舞台局部重绘契约 —— 哨兵法验证脏矩形之外
  // 一个像素都不许写，且脏矩形不许越过舞台边界 [0..UI_W, 38..332]。
  // 巨字时间背景一旦与脏矩形相交就必须整块重画，否则会留下被清成底色的黑洞。
  {
    ui::setActiveTheme(ui::UiTheme::TIDE);
    NovaState st;
    populateSample(st);
    st.run = 0; st.wait = 0;
    st.activeModel[0] = '\0'; st.msg[0] = '\0';
    NovaSettings cfg;
    nav.setPage(UiPage::HOME);
    nvaSetState(NvaAnimKey::SLEEP, false, true);
    for (int i = 0; i < 6; i++) nvaTick(6500u + (uint32_t)i * 120u);
    nav.draw(canvas, st, cfg, 24, 6500);
    pageHomeTap(184, 220, st);
    nvaSetState(NvaAnimKey::SLEEP, false, true);
    for (int i = 0; i < 4; i++) nvaTick(7300u + (uint32_t)i * 120u);

    constexpr uint16_t kSentinel = 0xA55A;
    uint16_t* fb = canvas.getFramebuffer();
    const uint8_t burnSteps[4] = { 0, 7, 13, 21 };
    for (int step = 0; step < 4; step++) {
      st.burnInStep = burnSteps[step];
      st.nowMs = 10100u + (uint32_t)step * 100u;
      for (size_t i = 0; i < (size_t)UI_W * UI_H; i++) fb[i] = kSentinel;
      int16_t dx = 0, dy = 0, dw = 0, dh = 0;
      pageHomeDrawStage(canvas, st, cfg, (uint32_t)(25 + step), &dx, &dy, &dw, &dh);
      size_t outside = 0, inside = 0;
      int fx = -1, fy = -1;
      for (int y = 0; y < UI_H; y++) {
        for (int x = 0; x < UI_W; x++) {
          const bool in = (x >= dx && x < dx + dw && y >= dy && y < dy + dh);
          const size_t idx = (size_t)y * UI_W + x;
          if (in) { if (fb[idx] != kSentinel) inside++; }
          else if (fb[idx] != kSentinel) { if (outside == 0) { fx = x; fy = y; } outside++; }
        }
      }
      if (outside != 0 || inside == 0 || dx < 0 || dy < 38 || dx + dw > UI_W || dy + dh > 338) {
        std::printf("tide_home_stage_local: FAIL step=%d outside=%zu inside=%zu rect=%d,%d %dx%d first=%d,%d\n",
                    step, outside, inside, dx, dy, dw, dh, fx, fy);
        ui::setActiveTheme(ui::UiTheme::OCEAN);
        std::fclose(manifest);
        return 1;
      }
      std::printf("tide_home_stage_local: ok step=%d inside=%zu rect=%d,%d %dx%d\n",
                  step, inside, dx, dy, dw, dh);
    }
    ui::setActiveTheme(ui::UiTheme::OCEAN);
    nav.setPage(UiPage::HOME);
    nvaSetState(NvaAnimKey::IDLE, false, true);
  }

  // 主屏交互与状态映射自检：覆盖工作姿势分类、睡醒惊醒、4 动作轮换、8 秒连戳傲娇与胶囊烧屏偏移。
  {
    auto failIf = [&](bool ok, const char* msg) -> bool {
      if (!ok) {
        std::printf("home_logic check failed: %s\n", msg);
        return true;
      }
      return false;
    };
    if (failIf(pageHomeWorkAnimForTool("") == NvaAnimKey::THINKING, "empty tool -> THINKING") ||
        failIf(pageHomeWorkAnimForTool("edit_file") == NvaAnimKey::CODING, "edit_file -> CODING") ||
        failIf(pageHomeWorkAnimForTool("BASH") == NvaAnimKey::CODING, "BASH -> CODING") ||
        failIf(pageHomeWorkAnimForTool("Write") == NvaAnimKey::CODING, "Write -> CODING") ||
        failIf(pageHomeWorkAnimForTool("grep_search") == NvaAnimKey::ARCHIVE, "grep_search -> ARCHIVE") ||
        failIf(pageHomeWorkAnimForTool("ReadFile") == NvaAnimKey::ARCHIVE, "ReadFile -> ARCHIVE") ||
        failIf(pageHomeWorkAnimForTool("run_command") == NvaAnimKey::CODING, "run_command -> CODING") ||
        failIf(pageHomeWorkAnimForTool("replace_file_content") == NvaAnimKey::CODING, "replace_file_content -> CODING") ||
        failIf(pageHomeWorkAnimForTool("view_file") == NvaAnimKey::ARCHIVE, "view_file -> ARCHIVE") ||
        failIf(pageHomeWorkAnimForTool("list_dir") == NvaAnimKey::ARCHIVE, "list_dir -> ARCHIVE") ||
        failIf(pageHomeWorkAnimForTool("browser") == NvaAnimKey::WORKING, "browser -> WORKING")) {
      std::fclose(manifest);
      return 1;
    }

    NovaState st;
    populateSample(st);
    st.run = 0;
    st.wait = 0;
    st.nowMs = 20000;

    // 睡眠 / 哈欠态点按宠物 -> WAKE_UP
    nvaSetState(NvaAnimKey::SLEEP, false, true);
    if (failIf(pageHomeTap(184, 220, st).extra == (int)NvaAnimKey::WAKE_UP, "SLEEP -> WAKE_UP")) {
      std::fclose(manifest);
      return 1;
    }
    st.nowMs = 30000;
    nvaSetState(NvaAnimKey::YAWN, false, true);
    if (failIf(pageHomeTap(184, 220, st).extra == (int)NvaAnimKey::WAKE_UP, "YAWN -> WAKE_UP")) {
      std::fclose(manifest);
      return 1;
    }

    // 清醒态间隔 >8s 连续点按：4 动作轮换且不连播同一个
    nvaSetState(NvaAnimKey::IDLE, false, true);
    int seenMask = 0;
    int prevTap = -1;
    for (int i = 0; i < 4; i++) {
      st.nowMs = 50000u + (uint32_t)i * 9000u;
      UiAction act = pageHomeTap(184, 220, st);
      bool inPool = (act.extra == (int)NvaAnimKey::CLICK ||
                     act.extra == (int)NvaAnimKey::CLICK_HAPPY ||
                     act.extra == (int)NvaAnimKey::CLICK_SHY ||
                     act.extra == (int)NvaAnimKey::CLICK_GIGGLE);
      if (failIf(act.kind == UiAction::PET_TAP && act.value == 0 && inPool && act.extra != prevTap,
                 "awake 4-tap rotation without repeat")) {
        std::fclose(manifest);
        return 1;
      }
      prevTap = act.extra;
      if (act.extra == (int)NvaAnimKey::CLICK) seenMask |= 1;
      else if (act.extra == (int)NvaAnimKey::CLICK_HAPPY) seenMask |= 2;
      else if (act.extra == (int)NvaAnimKey::CLICK_SHY) seenMask |= 4;
      else if (act.extra == (int)NvaAnimKey::CLICK_GIGGLE) seenMask |= 8;
    }
    if (failIf(seenMask == 0x0F, "all 4 click animations visited")) {
      std::fclose(manifest);
      return 1;
    }

    // 8 秒内连续戳 3 次 -> CLICK_ANGRY，并清零连击计数
    st.nowMs = 100000u;
    UiAction t1 = pageHomeTap(184, 220, st);
    st.nowMs = 102000u;
    UiAction t2 = pageHomeTap(184, 220, st);
    st.nowMs = 104000u;
    UiAction t3 = pageHomeTap(184, 220, st);
    st.nowMs = 105000u;
    UiAction t4 = pageHomeTap(184, 220, st);
    if (failIf(t1.extra != (int)NvaAnimKey::CLICK_ANGRY &&
               t2.extra != (int)NvaAnimKey::CLICK_ANGRY &&
               t3.extra == (int)NvaAnimKey::CLICK_ANGRY &&
               t4.extra != (int)NvaAnimKey::CLICK_ANGRY,
               "8s 3-hit combo triggers CLICK_ANGRY and resets")) {
      std::fclose(manifest);
      return 1;
    }

    // Demo 动作预览仅通过串口命令设置，日常点击状态胶囊 / TIDE 眉标不误触进入 Demo
    pageHomeSetDemoOverride(23);
    if (failIf(pageHomeDemoOverride() == 23, "demo override 23")) {
      std::fclose(manifest);
      return 1;
    }
    pageHomeSetDemoOverride(-1);
    st.burnInStep = 7;  // ox=+2, oy=+1
    UiAction pillTap = pageHomeTap(86 + 2, 308 + 1, st);
    if (failIf(pillTap.kind == UiAction::NONE && pageHomeDemoOverride() == -1,
               "OCEAN status pill tap does not enter demo")) {
      std::fclose(manifest);
      return 1;
    }

    // 睡眠 / 哈欠期间新到或进行中的 LLM 剧本不得拉起梦游走动
    NovaSettings choreoCfg;
    st.nChoreoSteps = 1;
    st.choreoGen = 99;
    st.choreoSteps[0].x = 268;
    st.choreoSteps[0].run = false;
    st.choreoSteps[0].anim = (uint8_t)NvaAnimKey::HUM;
    st.choreoSteps[0].durMs = 12000;
    std::strcpy(st.choreoSteps[0].say, "摸鱼哼歌~");
    nvaSetState(NvaAnimKey::SLEEP, false, true);
    st.nowMs = 110000u;
    pageHomeTick(st, choreoCfg, 100, true);
    if (failIf(!pageHomeIsWalking() && pageHomeChoreoAnim() == -1,
               "SLEEP blocks choreo sleepwalking")) {
      std::fclose(manifest);
      return 1;
    }

    // 清醒期导演剧本若编排了 sleep/yawn 提醒休息，应原地播完而不被判定为 resting 自毁重进
    st.choreoGen = 100;
    st.choreoSteps[0].x = 268;
    st.choreoSteps[0].run = true;
    st.choreoSteps[0].anim = (uint8_t)NvaAnimKey::SLEEP;
    st.choreoSteps[0].durMs = 12000;
    std::strcpy(st.choreoSteps[0].say, "夜深啦快去睡~");
    nvaSetState(NvaAnimKey::IDLE, false, true);
    st.nowMs = 112000u;
    pageHomeTick(st, choreoCfg, 101, false);
    nvaSetState((NvaAnimKey)pageHomeChoreoAnim(), false, true);
    nvaTick(112100u);
    st.nowMs = 112200u;
    pageHomeTick(st, choreoCfg, 102, false);
    if (failIf(!pageHomeIsWalking() &&
               pageHomeChoreoAnim() == (int)NvaAnimKey::SLEEP &&
               nvaActiveState() == NvaAnimKey::SLEEP,
               "choreo SLEEP plays in place without fighting resting")) {
      std::fclose(manifest);
      return 1;
    }

    // 空地点击触发快速小跑
    UiAction runAct = pageHomeTap(92, 200, st);
    if (failIf(runAct.kind == UiAction::PET_TAP && runAct.value == 1 &&
               pageHomeIsWalking() && pageHomeIsRunning(), "empty stage tap -> running")) {
      std::fclose(manifest);
      return 1;
    }

    // 导演剧本断点书签：播完当前步推进到下一步；断线重推同内容剧本时保持书签不回零
    pageHomeBookmarkAdvance(st, false);
    st.nChoreoSteps = 2;
    st.choreoGen = 101;
    st.choreoSteps[0].x = 184;
    st.choreoSteps[0].run = false;
    st.choreoSteps[0].anim = (uint8_t)NvaAnimKey::HUM;
    st.choreoSteps[0].durMs = 10000;
    std::strcpy(st.choreoSteps[0].say, "第一步哼歌");
    st.choreoSteps[1].x = 184;
    st.choreoSteps[1].run = false;
    st.choreoSteps[1].anim = (uint8_t)NvaAnimKey::BUBBLE;
    st.choreoSteps[1].durMs = 10000;
    std::strcpy(st.choreoSteps[1].say, "第二步冒泡");
    st.nowMs = 113000u;
    pageHomeSetDrainHold(false);
    pageHomeTick(st, choreoCfg, 103, false);
    if (failIf(pageHomeChoreoIdx() == 0 && pageHomeChoreoAnim() == (int)NvaAnimKey::HUM,
               "choreo starts at step 0")) {
      std::fclose(manifest);
      return 1;
    }
    pageHomeBookmarkAdvance(st, true);
    st.choreoGen = 102;  // 模拟重连重推同一份剧本
    st.nowMs = 113500u;
    pageHomeTick(st, choreoCfg, 104, false);
    if (failIf(pageHomeChoreoIdx() == 1 && pageHomeChoreoAnim() == (int)NvaAnimKey::BUBBLE,
               "identical choreo reconnect preserves bookmark step 1")) {
      std::fclose(manifest);
      return 1;
    }
    pageHomeBookmarkAdvance(st, false);

    // TIDE 主题左上眉标不误触进入 Demo，轻戳舞台仍走正常互动，不锁死演示模式
    ui::setActiveTheme(ui::UiTheme::TIDE);
    st.burnInStep = 0;
    UiAction tideTap = pageHomeTap(24, 160, st);
    if (failIf(tideTap.kind == UiAction::PET_TAP && tideTap.value == 1 &&
               pageHomeDemoOverride() == -1, "TIDE eyebrow tap stays out of demo")) {
      std::fclose(manifest);
      return 1;
    }
    pageHomeBookmarkAdvance(st, false);
    pageHomeSetDemoOverride(-1);
    ui::setActiveTheme(ui::UiTheme::OCEAN);

    // 跑步 / 螃蟹步到达屏幕边缘时自动折返换向，不原地贴墙空跑
    st.nChoreoSteps = 1;
    st.choreoGen = 105;
    st.choreoSteps[0].x = 288;  // 右边界 PET_MAX_X
    st.choreoSteps[0].run = true;
    st.choreoSteps[0].anim = (uint8_t)NvaAnimKey::HAPPY_RUN;
    st.choreoSteps[0].durMs = 15000;
    st.choreoSteps[0].say[0] = '\0';
    st.nowMs = 120000u;
    pageHomeTick(st, choreoCfg, 105, false);
    for (int f = 0; f < 60; f++) {
      st.nowMs = 120050u + (uint32_t)f * 50u;
      pageHomeTick(st, choreoCfg, (uint32_t)(106 + f), false);
    }
    if (failIf(pageHomeIsWalking() && pageHomeIsRunning(),
               "HAPPY_RUN bounces back at right wall instead of stopping/stuck")) {
      std::fclose(manifest);
      return 1;
    }
    pageHomeBookmarkAdvance(st, false);

    // AOD 局部熟睡脏矩形契约：只写包围盒内像素且严格保持 <= 64KB DMA 快传预算
    {
      constexpr uint16_t kSentinel = 0x5A3C;
      uint16_t* fb = canvas.getFramebuffer();
      nvaSetState(NvaAnimKey::SLEEP, false, true);
      for (int i = 0; i < 4; i++) nvaTick(150000u + (uint32_t)i * 120u);
      pageHomeDrawAod(canvas, st, 150500u);
      for (size_t i = 0; i < (size_t)UI_W * UI_H; i++) fb[i] = kSentinel;
      int16_t adx = 0, ady = 0, adw = 0, adh = 0;
      pageHomeDrawAodPet(canvas, st, 150700u, &adx, &ady, &adw, &adh);
      size_t outCnt = 0, inCnt = 0;
      for (int y = 0; y < UI_H; y++) {
        for (int x = 0; x < UI_W; x++) {
          const bool in = (x >= adx && x < adx + adw && y >= ady && y < ady + adh);
          const size_t idx = (size_t)y * UI_W + x;
          if (in) { if (fb[idx] != kSentinel) inCnt++; }
          else if (fb[idx] != kSentinel) outCnt++;
        }
      }
      if (failIf(outCnt == 0 && inCnt > 0 && adw > 0 && adh > 0 &&
                 (size_t)adw * (size_t)adh * 2u <= 65536u,
                 "AOD partial pet rect strict bounds and <=64KB DMA")) {
        std::fclose(manifest);
        return 1;
      }
    }

    // TIDE 白底气泡过期后下一帧必须完整清除气泡区域，不留残缺白块
    {
      ui::setActiveTheme(ui::UiTheme::TIDE);
      NovaState tst;
      populateSample(tst);
      tst.run = 0; tst.wait = 0;
      tst.burnInStep = 0;
      tst.nowMs = 160000u;
      NovaSettings tcfg;
      nav.setPage(UiPage::HOME);
      nvaSetState(NvaAnimKey::IDLE, false, true);
      nvaTick(160050u);
      pageHomeShowBubble("测试超长气泡不要残缺", 2000u, tst.nowMs);
      nav.draw(canvas, tst, tcfg, 1, tst.nowMs);
      tst.nowMs = 160500u;
      pageHomeDrawStage(canvas, tst, tcfg, 2, nullptr, nullptr, nullptr, nullptr);
      // 过期后画一帧：气泡区域右侧 (x=280..350, y=150..176) 不许残留任何纯白 (0xFFFF) 像素
      tst.nowMs = 163000u;
      pageHomeDrawStage(canvas, tst, tcfg, 3, nullptr, nullptr, nullptr, nullptr);
      uint16_t* fb = canvas.getFramebuffer();
      size_t strayWhite = 0;
      for (int y = 148; y < 180; y++) {
        for (int x = 270; x < 354; x++) {
          if (fb[(size_t)y * UI_W + x] == 0xFFFFu) strayWhite++;
        }
      }
      ui::setActiveTheme(ui::UiTheme::OCEAN);
      if (failIf(strayWhite == 0, "TIDE expired bubble leaves zero stray white pixels")) {
        std::fclose(manifest);
        return 1;
      }
    }
  }

  // ── Clawd 视觉出图与双宠切换契约：不进 51 项基线哈希，生成独立 PNG 供逐张验收 ─────
  {
    auto failIf = [&](bool ok, const char* msg) -> bool {
      if (!ok) {
        std::printf("clawd sim check failed: %s\n", msg);
        return true;
      }
      return false;
    };
    if (failIf(nvaPetHasAssets("assets/pets/clawd") && nvaSwitchRoot("assets/pets/clawd"),
               "nvaSwitchRoot assets/pets/clawd succeeds")) {
      std::fclose(manifest);
      return 1;
    }

    struct ClawdCase {
      const char* name;
      UiPage page;
      ui::UiTheme theme;
      bool aod;
      int8_t anim;
      uint8_t animTicks;
      uint8_t settingsTab;
      uint8_t openId;
      bool simulateTap;
    };
    ClawdCase ccases[] = {
      { "clawd_home_idle",    UiPage::HOME,     ui::UiTheme::OCEAN, false, (int8_t)NvaAnimKey::IDLE,    4, 0, 0,  false },
      { "clawd_home_click",   UiPage::HOME,     ui::UiTheme::OCEAN, false, (int8_t)NvaAnimKey::CLICK,   5, 0, 0,  true  },
      { "clawd_home_work",    UiPage::HOME,     ui::UiTheme::OCEAN, false, (int8_t)NvaAnimKey::WORKING, 5, 0, 0,  false },
      { "clawd_aod",          UiPage::HOME,     ui::UiTheme::OCEAN, true,  (int8_t)NvaAnimKey::SLEEP,   4, 0, 0,  false },
      { "clawd_tide_home",    UiPage::HOME,     ui::UiTheme::TIDE,  false, (int8_t)NvaAnimKey::CODING,  5, 0, 0,  false },
      { "clawd_settings_beh", UiPage::SETTINGS, ui::UiTheme::OCEAN, false, -1,                          0, 3, 24, false },
    };

    const ui::UiTheme prevTheme = ui::activeTheme();
    for (const auto& cc : ccases) {
      ui::setActiveTheme(cc.theme);
      NovaState st;
      populateSample(st);
      st.nowMs = 20000u;
      if (cc.anim == (int8_t)NvaAnimKey::IDLE) {
        st.run = 0; st.wait = 0; st.activeModel[0] = '\0'; st.msg[0] = '\0';
      }
      NovaSettings& cfg = settingsMut();
      cfg = NovaSettings{};
      cfg.petKind = 1;
      cfg.uiTheme = (cc.theme == ui::UiTheme::TIDE) ? 1 : 0;

      pageSettingsSetTab(cc.settingsTab);
      if (cc.openId) pageSettingsDebugOpen(cc.openId);
      nav.setPage(cc.page);

      if (cc.aod) {
        nvaSetState(NvaAnimKey::SLEEP, false, true);
        for (int i = 0; i < cc.animTicks; i++) nvaTick(st.nowMs + (uint32_t)i * 120u);
        pageHomeDrawAod(canvas, st, st.nowMs);
      } else {
        if (cc.simulateTap) {
          pageHomeShowBubble("hello, world!", 4000u, st.nowMs);
        }
        if (cc.anim >= 0) {
          nvaSetState((NvaAnimKey)cc.anim, false, true);
          for (uint8_t i = 0; i < cc.animTicks; i++) nvaTick(st.nowMs + (uint32_t)i * 120u);
        }
        nav.draw(canvas, st, cfg, 24, st.nowMs);
      }
      std::string path = outDir + "/" + cc.name + ".png";
      if (!saveRgb565Png(path.c_str(), canvas.getFramebuffer(), UI_W, UI_H)) return 1;
      std::printf("wrote %s\n", path.c_str());
    }

    settingsMut() = NovaSettings{};
    ui::setActiveTheme(prevTheme);
    if (failIf(nvaSwitchRoot("assets/pets/nova"), "switch back to assets/pets/nova")) {
      std::fclose(manifest);
      return 1;
    }
  }

  std::fclose(manifest);
  return 0;
}
