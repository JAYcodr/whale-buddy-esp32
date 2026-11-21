#pragma once
#include "anim/nva.h"
#include "model/settings.h"
#include "model/state.h"
#include "ui/canvas.h"
#include <stdint.h>

enum class UiPage : uint8_t {
  HOME     = 0,
  QUOTA    = 1,
  RANK     = 2,
  DETAIL   = 3,
  SETTINGS = 4,
};

struct UiAction {
  enum Kind : uint8_t {
    NONE = 0,
    GO_PAGE,
    SET_HOME,
    SET_POWER,
    TOGGLE_SOUND,
    SET_BRIGHT,
    REPROVISION,
    CANCEL_PORTAL,
    PET_TAP,
    APPROVE,
    DENY,
    TO_PC,
    RANK_REDRAW,
  };
  Kind kind = NONE;
  int  value = 0;
};

void pageHomeDraw(NovaCanvas& c, const NovaState& st, uint32_t frame);
void pageHomeDrawStage(NovaCanvas& c, const NovaState& st, uint32_t frame,
                       int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);
UiAction pageHomeTap(int16_t x, int16_t y, const NovaState& st);
int pageHomeDemoOverride();  // -1 = auto, >=0 = NvaAnimKey index
int pageHomeChoreoAnim();    // -1 = none, >=0 = NvaAnimKey index from LLM script
bool pageHomeIsWalking();
void pageHomeTriggerSquash(uint32_t nowMs);

void pageQuotaDraw(NovaCanvas& c, const NovaState& st);
UiAction pageQuotaTap(int16_t x, int16_t y);

void pageRankDraw(NovaCanvas& c, const NovaState& st);
UiAction pageRankTap(int16_t x, int16_t y);
bool pageRankScrollPx(int16_t deltaY, uint8_t nRows);
void pageRankResetScroll();

void pageDetailDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg);
UiAction pageDetailTap(int16_t x, int16_t y);

void pageSettingsDraw(NovaCanvas& c, const NovaSettings& cfg);
UiAction pageSettingsTap(int16_t x, int16_t y, const NovaSettings& cfg);

void pageApprovalDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
UiAction pageApprovalTap(int16_t x, int16_t y);
