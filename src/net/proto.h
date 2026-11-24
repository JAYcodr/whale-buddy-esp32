#pragma once
#include "model/settings.h"
#include "model/state.h"
#include <stddef.h>
#include <stdint.h>

// Apply an incoming MQTT message for topic suffix ("state", "sessions", "quota",
// "rank", "tokens", "approval", "approval_end", "time", "choreo", "tts", "mic_ack", "chat").
// Thread-safe on NovaState.
bool protoApplyMessage(const char* suffix, const char* payload, size_t len, uint32_t nowMs);

// Outbound payload builders
struct PowerEvent;
size_t protoBuildHello(char* buf, size_t cap, const NovaState& st, const NovaSettings& cfg, bool online);
size_t protoBuildDecision(char* buf, size_t cap, const char* id, bool accept, bool toPc);
size_t protoBuildStat(char* buf, size_t cap, const NovaState& st);
size_t protoBuildPrefs(char* buf, size_t cap, const NovaSettings& cfg);
size_t protoBuildPowerEvent(char* buf, size_t cap, const PowerEvent& ev);
size_t protoBuildChatSend(char* buf, size_t cap, uint16_t id, const char* text,
                          const char* target = nullptr, const char* mode = nullptr);
size_t protoBuildChatTts(char* buf, size_t cap, const char* text);
