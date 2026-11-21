#pragma once
#include "model/state.h"
#include <stddef.h>
#include <stdint.h>

// Apply an incoming MQTT message for topic suffix ("state", "quota", "rank",
// "approval", "approval_end", "time"). Thread-safe on NovaState.
bool protoApplyMessage(const char* suffix, const char* payload, size_t len, uint32_t nowMs);

// Outbound payload builders
size_t protoBuildHello(char* buf, size_t cap, const NovaState& st, bool online);
size_t protoBuildDecision(char* buf, size_t cap, const char* id, bool accept, bool toPc);
size_t protoBuildStat(char* buf, size_t cap, const NovaState& st);
