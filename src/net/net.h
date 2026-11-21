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
void netPublishDecision(const char* approvalId, bool accept, bool toPc);
void netPublishPrefs();
void netPublishSync();
void netSetPowerSave(NetPsMode mode);
