#pragma once
#include "anim/nva.h"
#include "model/settings.h"
#include "model/state.h"
#include "ui/canvas.h"
#include "ui/theme.h"
#include <stdint.h>

enum class UiPage : uint8_t {
  HOME     = 0,
  QUOTA    = 1,
  RANK     = 2,
  DETAIL   = 3,
  SETTINGS = 4,
  TOKENS   = 5,
};

struct UiAction {
  enum Kind : uint8_t {
    NONE = 0,
    GO_PAGE,
    SET_HOME,
    SET_POWER,
    SET_SOUND,
    SET_BRIGHT,
    SET_TTS,
    SET_QUIET,
    SET_BATSLEEP,
    SET_CHGLIMIT,
    SET_CHGSPEED,
    SET_AOD_MODE,
    SET_AOD_IDLE,
    SET_AOD_NIGHT,
    SET_CHOREO,
    TEST_SOUND,
    SYNC_HOST,
    REPROVISION,
    CANCEL_PORTAL,
    PET_TAP,
    APPROVE,
    DENY,
    TO_PC,
    RANK_REDRAW,
    SETTINGS_REDRAW,
    TOKENS_REDRAW,
    DOCK_TOGGLE,   // 主屏码头：展开/收起异常详情
  };
  Kind kind = NONE;
  int  value = 0;
};

void pageHomeDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame);
void pageHomeDrawStage(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame,
                       int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);
UiAction pageHomeTap(int16_t x, int16_t y, const NovaState& st);
void pageHomeDrawAod(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
int pageHomeDemoOverride();  // -1 = auto, >=0 = NvaAnimKey index
void pageHomeSetDemoOverride(int idx);
int pageHomeChoreoAnim();    // -1 = none, >=0 = NvaAnimKey index from LLM script
bool pageHomeIsWalking();
void pageHomeTriggerSquash(uint32_t nowMs);

void pageQuotaDraw(NovaCanvas& c, const NovaState& st);
UiAction pageQuotaTap(int16_t x, int16_t y, const NovaState& st);
bool pageQuotaTick(uint32_t nowMs);

void pageRankDraw(NovaCanvas& c, const NovaState& st);
void pageRankDrawList(NovaCanvas& c, const NovaState& st);
UiAction pageRankTap(int16_t x, int16_t y);
bool pageRankScrollPx(int16_t deltaY, uint8_t nRows);
void pageRankResetScroll();
void pageRankTouchDown(int16_t y, uint32_t ms);
bool pageRankTouchMove(int16_t y, uint32_t ms, uint8_t nRows);
void pageRankTouchUp(uint32_t ms, uint8_t nRows);
bool pageRankTick(uint32_t nowMs);
bool pageRankIsFlinging();

void pageDetailDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg);
UiAction pageDetailTap(int16_t x, int16_t y);

void pageSettingsDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg);
UiAction pageSettingsTap(int16_t x, int16_t y, const NovaSettings& cfg);
void pageSettingsSetTab(uint8_t tab);
bool pageSettingsTick(uint32_t nowMs);

void pageApprovalDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
void pageApprovalDrawBar(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
UiAction pageApprovalTap(int16_t x, int16_t y);

void pageTokensDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
void pageTokensDrawStage(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t nowMs);
UiAction pageTokensTap(int16_t x, int16_t y, const NovaState& st, uint32_t nowMs);
void pageTokensOnEnter(uint32_t nowMs);
bool pageTokensIsAnimating(const NovaState& st, const NovaSettings& cfg, uint32_t nowMs);

int uiPageRingIndex(UiPage p);

bool pageHomeHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageQuotaHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageRankHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageDetailHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageSettingsHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageApprovalHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageTokensHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
