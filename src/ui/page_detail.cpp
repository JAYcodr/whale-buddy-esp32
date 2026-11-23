#include "ui/pages.h"
#include "ui/theme.h"
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
}  // namespace

void pageDetailDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg) {
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
