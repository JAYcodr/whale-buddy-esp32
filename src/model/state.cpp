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
