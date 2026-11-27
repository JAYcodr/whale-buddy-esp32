#include "model/state.h"
#include <stdio.h>
#include <string.h>

#ifndef NOVA_HOST_SIM
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace {
NovaState s_state;
#ifndef NOVA_HOST_SIM
SemaphoreHandle_t s_mtx = nullptr;
#endif
}  // namespace

void novaStateInit(const char* devId, const char* fwVer) {
#ifndef NOVA_HOST_SIM
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
#endif
  novaStateLock();
  memset(&s_state, 0, sizeof(s_state));
  if (devId) strncpy(s_state.deviceId, devId, sizeof(s_state.deviceId) - 1);
  if (fwVer) strncpy(s_state.fwVer, fwVer, sizeof(s_state.fwVer) - 1);
  s_state.ctxPct = -1;
  s_state.recGainDb = 24;
  s_state.recPeakDb = -96.0f;
  s_state.recRmsDb = -96.0f;
  strncpy(s_state.msg, "等待连接 DSH 宿主...", sizeof(s_state.msg) - 1);
  strncpy(s_state.rankSrc, "ModelDial", sizeof(s_state.rankSrc) - 1);
  strncpy(s_state.rankLic, "CC BY 4.0", sizeof(s_state.rankLic) - 1);
  novaStateUnlock();
}

void novaStateLock() {
#ifndef NOVA_HOST_SIM
  if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
#endif
}

void novaStateUnlock() {
#ifndef NOVA_HOST_SIM
  if (s_mtx) xSemaphoreGive(s_mtx);
#endif
}

NovaState& novaStateUnsafeRef() { return s_state; }

void novaStateRead(NovaState* out) {
  if (!out) return;
  novaStateLock();
  *out = s_state;
  novaStateUnlock();
}

void novaSafeUtf8Copy(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t i = 0;
  while (src[i]) {
    uint8_t b = (uint8_t)src[i];
    size_t len = 1;
    if ((b & 0xE0) == 0xC0) len = 2;
    else if ((b & 0xF0) == 0xE0) len = 3;
    else if ((b & 0xF8) == 0xF0) len = 4;
    if (i + len >= cap) break;
    bool valid = true;
    for (size_t k = 1; k < len; k++) {
      if (((uint8_t)src[i + k] & 0xC0) != 0x80) { valid = false; break; }
    }
    if (!valid) break;
    i += len;
  }
  memcpy(dst, src, i);
  dst[i] = '\0';
}

void novaChatAppend(NovaState& st, bool isUser, uint16_t id, const char* text) {
  if (!text || !text[0]) return;
  if (st.nChatMsgs >= MAX_CHAT_MSGS) {
    memmove(&st.chatMsgs[0], &st.chatMsgs[1],
            (size_t)(MAX_CHAT_MSGS - 1) * sizeof(ChatMsg));
    st.nChatMsgs = MAX_CHAT_MSGS - 1;
  }
  ChatMsg& m = st.chatMsgs[st.nChatMsgs++];
  m.isUser = isUser;
  m.id = id;
  novaSafeUtf8Copy(m.text, sizeof(m.text), text);
}

bool novaRecCheckWaitTimeout(NovaState& st, uint32_t nowMs) {
  if ((st.recState != 3 && st.recState != 4) || st.recWaitStartMs == 0) return false;
  const bool waitAck = (st.recState == 3);
  const uint32_t timeoutMs = waitAck ? NOVA_REC_ACK_TIMEOUT_MS : NOVA_REC_ASR_TIMEOUT_MS;
  if (nowMs < st.recWaitStartMs || (uint32_t)(nowMs - st.recWaitStartMs) < timeoutMs) return false;
  st.recState = 5;
  st.recWaitStartMs = 0;
  snprintf(st.recErr, sizeof(st.recErr), "%s", waitAck ? "回执超时" : "识别超时");
  st.recGen++;
  return true;
}

const BoardSession* novaBoardFind(const NovaState& st, const char* id) {
  if (!id || !id[0]) return nullptr;
  const uint8_t n = (st.nBoardSessions <= MAX_BOARD_SESSIONS)
                      ? st.nBoardSessions
                      : (uint8_t)MAX_BOARD_SESSIONS;
  for (uint8_t i = 0; i < n; i++) {
    if (strcmp(st.boardSessions[i].id, id) == 0) {
      return &st.boardSessions[i];
    }
  }
  return nullptr;
}

const BoardSession* novaBoardDefaultTarget(const NovaState& st) {
  const uint8_t n = (st.nBoardSessions <= MAX_BOARD_SESSIONS)
                      ? st.nBoardSessions
                      : (uint8_t)MAX_BOARD_SESSIONS;
  for (uint8_t i = 0; i < n; i++) {
    if (st.boardSessions[i].state == BoardSessState::IDLE) return &st.boardSessions[i];
  }
  for (uint8_t i = 0; i < n; i++) {
    if (st.boardSessions[i].state == BoardSessState::ERR) return &st.boardSessions[i];
  }
  for (uint8_t i = 0; i < n; i++) {
    if (st.boardSessions[i].state == BoardSessState::RUN) return &st.boardSessions[i];
  }
  return nullptr;
}

const BoardSession* novaBoardCycleTarget(const NovaState& st, const char* curId) {
  const uint8_t n = (st.nBoardSessions <= MAX_BOARD_SESSIONS)
                      ? st.nBoardSessions
                      : (uint8_t)MAX_BOARD_SESSIONS;
  int curIdx = -1;
  if (curId && curId[0]) {
    for (uint8_t i = 0; i < n; i++) {
      if (strcmp(st.boardSessions[i].id, curId) == 0) {
        curIdx = (int)i;
        break;
      }
    }
  }
  for (uint8_t step = 1; step <= n; step++) {
    const uint8_t idx = (uint8_t)((curIdx + (int)step) % (int)n);
    if (st.boardSessions[idx].state != BoardSessState::WAIT) {
      return &st.boardSessions[idx];
    }
  }
  return nullptr;
}
