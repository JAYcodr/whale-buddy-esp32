#pragma once
#include <stdint.h>

enum class NetPsMode : uint8_t {
  LOW_POWER = 0,   // WIFI_PS_MAX_MODEM
  PERFORMANCE = 1, // WIFI_PS_NONE
};

bool netStart();
void netRequestReprovision();
void netRequestCancelPortal();
void netRequestWifiConnect(const char* ssid, const char* pass);
struct PowerEvent;
void netPublishDecision(const char* approvalId, bool accept, bool toPc);
void netPublishPrefs();
void netPublishSync();
bool netPublishPowerEvent(const PowerEvent& ev);
void netSetPowerSave(NetPsMode mode);
void netSetStatIntervalMs(uint32_t ms);   // 5..300s，0=回默认 30s
void netSetPowerState(bool usbPresent);   // 电池下按 RSSI 降发射功率
int8_t netTxPowerApplied();               // 0.25dBm 单位；-1 = 还没设过
uint32_t netPublishQueueDropCount();      // 出站队列满导致的丢弃次数
