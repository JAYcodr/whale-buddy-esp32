#include "ui/pages.h"
#include "ui/theme.h"
#include "ui/tide_palette.h"
#include <stdio.h>

namespace {
constexpr ui::Rect BTN_SETTINGS{ 20, 358, 328, 48 };

void drawKvRow(NovaCanvas& c, int16_t x, int16_t y, int16_t w,
               const char* k, const char* v, uint16_t valCol, bool divider) {
  ui::drawText(c, ui::fontCjk14(), x + 16, y, k, ui::COL_DIM, ui::COL_CARD);
  char valBuf[64];
  ui::fitUtf8Line(c, ui::fontCjk14(), v, w - 114, valBuf, sizeof(valBuf));
  ui::drawTextRight(c, ui::fontCjk14(), x + w - 16, y, valBuf, valCol, ui::COL_CARD);
  if (divider) {
    c.drawFastHLine(x + 16, y + 10, w - 32, rgb565(20, 32, 52));
  }
}
// ── 潮汐（TIDE）设备心跳 ──────────────────────────────────────────────
// 契约 §4⑤：在线钴蓝 / 断线信号红；顶部双列巨数遥测头；中部 5 行纯白细线清单；
// 底部主按钮复用 BTN_SETTINGS 热区画成纯白实心 0px 锐角大钮。
constexpr int16_t TIDE_HEAD_TOP  = 42;
constexpr int16_t TIDE_HEAD_BOT  = 152;
constexpr int16_t TIDE_LIST_TOP  = 160;
constexpr int16_t TIDE_ROW_H     = 37;

// 一行「左标签 / 右值」，中间一条 1px 自适应柔和分割线（无卡片、绝不串蓝）。
void drawTideKvRow(NovaCanvas& c, int16_t baseY, const char* k, const char* v,
                   uint16_t valCol, uint16_t bg, bool divider, const AaFont* valFont = nullptr) {
  const uint16_t dim = ui::tide::dimOn(bg);
  const uint16_t ln  = ui::tide::lineOn(bg);
  ui::drawText(c, ui::fontCjk14(), UI_SAFE_L, baseY, k, dim, bg);
  const AaFont* f = valFont ? valFont : ui::fontCjk14();
  char valBuf[64];
  ui::fitUtf8Line(c, f, v, UI_SAFE_W - 96, valBuf, sizeof(valBuf));
  ui::drawTextRight(c, f, UI_SAFE_R, baseY, valBuf, valCol, bg);
  if (divider) c.fillRect(UI_SAFE_L, (int16_t)(baseY + 12), UI_SAFE_W, 1, ln);
}

void drawDetailTide(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg) {
  const bool off = (!st.wifiConnected || !st.mqttConnected);
  const uint16_t bg  = off ? ui::tide::BG_CRIMSON : ui::tide::BG_COBALT;
  const uint16_t dim = ui::tide::dimOn(bg);
  const uint16_t ln  = ui::tide::lineOn(bg);
  c.fillScreen(bg);
  ui::tide::drawTopStrip(c, st, 4, bg);

  // ── 顶部双列巨数遥测头 ──
  ui::drawText(c, ui::fontAscii14(), UI_SAFE_L, 53,
               off ? "LINK LOST" : "BATTERY", dim, bg);
  if (off) {
    // 断线态不再把 "OFF" 塞给纯数字字库导致萎缩成小字，改为高对比纯白实心大章。
    const ui::Rect offBox{ UI_SAFE_L, 68, 126, 48 };
    c.fillRect(offBox.x, offBox.y, offBox.w, offBox.h, ui::tide::WHITE);
    ui::drawTextCenteredInRect(c, ui::fontNum24(), offBox, "OFF", bg, ui::tide::WHITE);
  } else {
    char big[16];
    snprintf(big, sizeof(big), "%u", (unsigned)st.battPct);
    ui::drawText(c, ui::fontNum92(), UI_SAFE_L, 126, big, ui::tide::WHITE, bg);
    ui::TextMetrics bm = ui::measureText(c, ui::fontNum92(), big);
    ui::drawText(c, ui::fontNum24(), (int16_t)(UI_SAFE_L + bm.w + 3), 124, "%", dim, bg);
  }

  char subL[40];
  if (off) snprintf(subL, sizeof(subL), "电池 %u%% · %.2fV", (unsigned)st.battPct, st.battMv / 1000.0f);
  else     snprintf(subL, sizeof(subL), "%.2fV · %umA", st.battMv / 1000.0f, (unsigned)st.chgMa);
  ui::drawText(c, ui::fontCjk14(), UI_SAFE_L, 145, subL, dim, bg);

  // 中间 1px 自适应竖线隔开右列
  const int16_t splitX = (int16_t)(UI_SAFE_L + UI_SAFE_W / 2 + 12);
  c.fillRect(splitX, TIDE_HEAD_TOP + 2, 1, (int16_t)(TIDE_HEAD_BOT - TIDE_HEAD_TOP - 10), ln);

  const int16_t colR = (int16_t)(splitX + 14);
  ui::drawText(c, ui::fontAscii14(), colR, 53, "RENDER", dim, bg);
  char fpsBuf[16];
  snprintf(fpsBuf, sizeof(fpsBuf), "%.0f", st.fps);
  ui::drawText(c, ui::fontNum46(), colR, 114, fpsBuf, ui::tide::WHITE, bg);
  {
    ui::TextMetrics fm = ui::measureText(c, ui::fontNum46(), fpsBuf);
    ui::drawText(c, ui::fontCjk14(), (int16_t)(colR + fm.w + 4), 114, "fps", dim, bg);
  }
  char subR[40];
  snprintf(subR, sizeof(subR), "%.1fms · %uMHz", st.pushUs / 1000.0f, (unsigned)st.cpuMhz);
  ui::drawText(c, ui::fontCjk14(), colR, 144, subR, dim, bg);

  c.fillRect(0, (int16_t)(TIDE_HEAD_BOT - 2), UI_W, 2, ui::tide::WHITE);

  // ── 中部 5 行清单 ──
  int16_t y = (int16_t)(TIDE_LIST_TOP + 16);

  char devBuf[48];
  snprintf(devBuf, sizeof(devBuf), "Nova-%s v%s", st.deviceId, st.fwVer);
  drawTideKvRow(c, y, "设备标识", devBuf, ui::tide::WHITE, bg, true);
  y = (int16_t)(y + TIDE_ROW_H);

  char wifiBuf[48];
  if (st.wifiConnected) snprintf(wifiBuf, sizeof(wifiBuf), "%s (%ddBm)", st.ip, (int)st.rssi);
  else if (st.wifiApMode) snprintf(wifiBuf, sizeof(wifiBuf), "AP 配网模式");
  else snprintf(wifiBuf, sizeof(wifiBuf), "未连接");
  drawTideKvRow(c, y, "WiFi 网络", wifiBuf, ui::tide::WHITE, bg, true);
  y = (int16_t)(y + TIDE_ROW_H);

  // MQTT 行：值右侧带纯白反色状态章
  {
    ui::drawText(c, ui::fontCjk14(), UI_SAFE_L, y, "MQTT 服务", dim, bg);
    const char* stamp = st.mqttConnected ? "在线" : "断开";
    const uint16_t stampCol = st.mqttConnected ? ui::tide::BG_COBALT : ui::tide::BG_CRIMSON;
    ui::TextMetrics sm = ui::measureText(c, ui::fontCjk14(), stamp);
    const int16_t sw = (int16_t)(sm.w + 16);
    const int16_t sx = (int16_t)(UI_SAFE_R - sw);
    c.fillRect(sx, (int16_t)(y - 17), sw, 24, ui::tide::WHITE);
    ui::drawTextCenteredInRect(c, ui::fontCjk14(), { sx, (int16_t)(y - 17), sw, 24 },
                               stamp, stampCol, ui::tide::WHITE);

    char mqttBuf[64];
    snprintf(mqttBuf, sizeof(mqttBuf), "%s:%u", cfg.brokerHost, (unsigned)cfg.brokerPort);
    char mqttFit[64];
    ui::fitUtf8Line(c, ui::fontCjk14(), mqttBuf, UI_SAFE_W - sw - 110, mqttFit, sizeof(mqttFit));
    ui::drawTextRight(c, ui::fontCjk14(), (int16_t)(sx - 10), y, mqttFit, ui::tide::WHITE, bg);
    c.fillRect(UI_SAFE_L, (int16_t)(y + 12), UI_SAFE_W, 1, ln);
  }
  y = (int16_t)(y + TIDE_ROW_H);

  drawTideKvRow(c, y, "当前模型", st.activeModel[0] ? st.activeModel : "待同步",
                ui::tide::WHITE, bg, true);
  y = (int16_t)(y + TIDE_ROW_H);

  drawTideKvRow(c, y, "主题策略", "潮汐亮屏 · AOD 回纯黑", ui::tide::WHITE, bg, false);

  // ── 底部主按钮：复用 BTN_SETTINGS 热区，纯白实心 0px 锐角大钮 ──
  c.fillRect(BTN_SETTINGS.x, BTN_SETTINGS.y, BTN_SETTINGS.w, BTN_SETTINGS.h, ui::tide::WHITE);
  ui::drawTextCenteredInRect(c, ui::fontCjk16(), BTN_SETTINGS,
                             off ? "打开设置 / 重新配网" : "系统与偏好设置", bg, ui::tide::WHITE);
  ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 428, "Key1 短按回首页 · 长按打开设置",
                       dim, bg);
}

}  // namespace

void pageDetailDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg) {
  if (ui::isTideTheme()) {
    drawDetailTide(c, st, cfg);
    return;
  }
  c.fillScreen(ui::COL_BG);
  ui::drawTopBar(c, st, 4);

  // Card 1: Hardware & Network
  ui::drawCard(c, 20, 66, 328, 136, ui::COL_CARD, ui::COL_BORDER, 18);
  ui::drawText(c, ui::fontCjk16(), 36, 93, "设备与连接", ui::COL_ACCENT, ui::COL_CARD);

  char devBuf[48];
  snprintf(devBuf, sizeof(devBuf), "Nova-%s v%s", st.deviceId, st.fwVer);
  drawKvRow(c, 20, 123, 328, "设备标识", devBuf, ui::COL_TEXT, true);

  char wifiBuf[48];
  if (st.wifiConnected) snprintf(wifiBuf, sizeof(wifiBuf), "%s (%ddBm)", st.ip, (int)st.rssi);
  else if (st.wifiApMode) snprintf(wifiBuf, sizeof(wifiBuf), "AP 配网模式");
  else snprintf(wifiBuf, sizeof(wifiBuf), "未连接");
  drawKvRow(c, 20, 155, 328, "WiFi 网络", wifiBuf, st.wifiConnected ? ui::COL_MINT : ui::COL_AMBER, true);

  char mqttBuf[64];
  snprintf(mqttBuf, sizeof(mqttBuf), "%s:%u %s",
           cfg.brokerHost, (unsigned)cfg.brokerPort, st.mqttConnected ? "在线" : "断开");
  drawKvRow(c, 20, 187, 328, "MQTT 服务", mqttBuf, st.mqttConnected ? ui::COL_MINT : ui::COL_RED, false);

  // Card 2: Runtime & Telemetry
  ui::drawCard(c, 20, 212, 328, 136, ui::COL_CARD, ui::COL_BORDER, 18);
  ui::drawText(c, ui::fontCjk16(), 36, 239, "运行与遥测", ui::COL_MINT, ui::COL_CARD);

  drawKvRow(c, 20, 269, 328, "当前模型", st.activeModel[0] ? st.activeModel : "待同步", ui::COL_TEXT, true);

  char pwrBuf[64];
  const BattStage bs = (BattStage)st.battStage;
  const char* pwrLabel = "电池";
  char chgLabel[24];
  switch (bs) {
    case BattStage::PRE:      pwrLabel = "预充中"; break;
    case BattStage::CC:       snprintf(chgLabel, sizeof(chgLabel), "充电%umA", (unsigned)st.chgMa); pwrLabel = chgLabel; break;
    case BattStage::CV:       pwrLabel = "恒压充电"; break;
    case BattStage::DONE:     pwrLabel = "已充满"; break;
    case BattStage::LIMITED: {
      const bool capPause = !st.chargeTempCapped &&
                            (cfg.chargeLimitPct < 100) &&
                            (st.battPct + 1 >= cfg.chargeLimitPct);
      pwrLabel = capPause ? "保养停充" : "高温停充";
      break;
    }
    case BattStage::USB_IDLE: pwrLabel = "USB供电"; break;
    case BattStage::DISCHARGE:pwrLabel = "电池"; break;
    default:                  pwrLabel = "未接电池"; break;
  }
  snprintf(pwrBuf, sizeof(pwrBuf), "%u%% · %.2fV · %s",
           (unsigned)st.battPct, st.battMv / 1000.0f, pwrLabel);
  drawKvRow(c, 20, 301, 328, "电源状态", pwrBuf, ui::COL_TEXT, true);

  // 静帧不报 0fps：活跃时报实测帧率 + 单帧 push 耗时，静帧时报静了多久。
  char perfBuf[64];
  const uint32_t idleMs = (st.nowMs > st.lastDrawMs) ? (st.nowMs - st.lastDrawMs) : 0;
  if (st.aodActive) {
    snprintf(perfBuf, sizeof(perfBuf), "AOD · 亮%u%% · %uMHz",
             (unsigned)st.effBright, (unsigned)st.cpuMhz);
  } else if (st.fps > 0.5f) {
    snprintf(perfBuf, sizeof(perfBuf), "%.0ffps · %.1fms · %uMHz",
             st.fps, st.pushUs / 1000.0f, (unsigned)st.cpuMhz);
  } else if (idleMs >= 3000u) {
    snprintf(perfBuf, sizeof(perfBuf), "静帧 %lus · 亮%u%%",
             (unsigned long)(idleMs / 1000u), (unsigned)st.effBright);
  } else {
    snprintf(perfBuf, sizeof(perfBuf), "就绪 · 亮%u%% · %uMHz",
             (unsigned)st.effBright, (unsigned)st.cpuMhz);
  }
  drawKvRow(c, 20, 333, 328, "系统性能", perfBuf, ui::COL_DIM, false);

  ui::drawButton(c, BTN_SETTINGS, "系统与偏好设置", false, ui::COL_ACCENT);
  ui::drawTextCentered(c, ui::fontCjk14(), UI_W / 2, 428, "Key1 短按回首页 · 长按打开设置", ui::COL_MUTED, ui::COL_BG);
}

UiAction pageDetailTap(int16_t x, int16_t y) {
  if (BTN_SETTINGS.contains(x, y)) return UiAction{ UiAction::GO_PAGE, (int)UiPage::SETTINGS };
  return UiAction{};
}

bool pageDetailHitRect(int16_t x, int16_t y, const NovaState&, const NovaSettings&, ui::Rect* out) {
  if (!out) return false;
  if (BTN_SETTINGS.contains(x, y)) {
    *out = BTN_SETTINGS;
    return true;
  }
  return false;
}
