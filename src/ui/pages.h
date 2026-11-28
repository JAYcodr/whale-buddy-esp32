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
  CHAT     = 6,
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
    SET_MIC_GAIN,
    SET_THEME,     // 界面风格：深海原版 / 深蓝潮汐
    SET_PET,       // 桌面宠物：诺瓦 / Clawd
    RESTORE_DEFAULTS,
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
    CHAT_REC_TOGGLE,
    CHAT_GAIN_CYCLE,
    CHAT_PLAY_TTS,
    CHAT_DRAFT_CANCEL,
    CHAT_DRAFT_SEND,
    CHAT_DRAFT_SEND_MODE,
    CHAT_DRAFT_CYCLE_TARGET,
    CHAT_SELECT_SESS,
    CHAT_DELIVERY_DISMISS,
    CHAT_REPLAY_LAST,
    CHAT_OPEN_APPROVAL,
    CHAT_QUICK_SEND,
  };
  Kind kind = NONE;
  int  value = 0;
  int  extra = 0;
  int  aux = 0;   // 附加参数：PET_TAP 点击时 = 语音条变体索引（0/1）
};

// 设置类 UiAction → kSettings：命中则校准后写入并 settingsSave()。
bool settingsApplyAction(UiAction::Kind kind, int value);

void pageHomeDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame);
void pageHomeDrawStage(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame,
                       int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);
void pageHomeTick(const NovaState& st, const NovaSettings& cfg, uint32_t frame, bool idleResting);
void pageHomeSetDrainHold(bool hold);
void pageHomeSetContinuousMove(uint8_t mode);  // 0=none, 1=crab_walk, 2=happy_run
void pageHomeBookmarkAdvance(const NovaState& st, bool advanceChoreo);
uint8_t pageHomeChoreoIdx();
void pageHomeShowBubble(const char* txt, uint32_t durMs, uint32_t nowMs);
UiAction pageHomeTap(int16_t x, int16_t y, const NovaState& st);
void pageHomeOnEnter(uint32_t nowMs);
void pageHomeOnExit();
void pageHomeOnThemeChange();
void pageHomeDrawAod(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
void pageHomeDrawAodPet(NovaCanvas& c, const NovaState& st, uint32_t nowMs,
                        int16_t* outDx, int16_t* outDy, int16_t* outDw, int16_t* outDh);
int pageHomeDemoOverride();  // -1 = auto, >=0 = NvaAnimKey index
void pageHomeSetDemoOverride(int idx);
int pageHomeChoreoAnim();    // -1 = none, >=0 = NvaAnimKey index from LLM script
bool pageHomeIsWalking();
bool pageHomeIsRunning();
NvaAnimKey pageHomeWorkAnimForTool(const char* tool);
void pageHomeTriggerSquash(uint32_t nowMs);

void pageQuotaDraw(NovaCanvas& c, const NovaState& st);
UiAction pageQuotaTap(int16_t x, int16_t y, const NovaState& st);
bool pageQuotaTick(uint32_t nowMs);
void pageQuotaOnExit();

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
void pageRankOnExit();

void pageDetailDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg);
UiAction pageDetailTap(int16_t x, int16_t y);

void pageSettingsDraw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg);
void pageSettingsDrawList(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg);
UiAction pageSettingsTap(int16_t x, int16_t y, const NovaSettings& cfg);
void pageSettingsSetTab(uint8_t tab);
void pageSettingsOnEnter();
void pageSettingsOnExit();
void pageSettingsCancelDrag();
bool pageSettingsTick(uint32_t nowMs);
bool pageSettingsIsAnimating();
bool pageSettingsIsScrolling();
ui::Rect pageSettingsListRect();
#ifdef NOVA_HOST_SIM
void pageSettingsDebugOpen(uint8_t id);
void pageSettingsDebugConfirm(uint8_t id);
#endif
// 滚动与滑块：设置页自己维护 scrollY、惯性/回弹与拖动状态，main 只转发触摸事件。
bool pageSettingsScrollBy(int16_t dy);
bool pageSettingsTouchDown(int16_t x, int16_t y, uint32_t nowMs);
bool pageSettingsTouchMove(int16_t x, int16_t y, uint32_t nowMs);
UiAction pageSettingsTouchUp(uint32_t nowMs);
bool pageSettingsScrollTouchMove(int16_t y, uint32_t nowMs);
void pageSettingsScrollTouchUp(uint32_t nowMs);
void pageSettingsScrollCancel();

void pageApprovalDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
void pageApprovalDrawBar(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
UiAction pageApprovalTap(int16_t x, int16_t y);

void pageTokensDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs);
void pageTokensDrawStage(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t nowMs);
void pageTokensDrawHud(NovaCanvas& c, const NovaState& st);
UiAction pageTokensTap(int16_t x, int16_t y, const NovaState& st, uint32_t nowMs);
void pageTokensOnEnter(uint32_t nowMs);
void pageTokensOnExit();
void pageTokensOnThemeChange();
bool pageTokensIsAnimating(const NovaState& st, const NovaSettings& cfg, uint32_t nowMs);

void pageChatDraw(NovaCanvas& c, const NovaState& st);
UiAction pageChatTap(int16_t x, int16_t y, const NovaState& st);
bool pageChatHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageChatIsHoldBtn(int16_t x, int16_t y, const NovaState& st);
bool pageChatCanStartHold(const NovaState& st);
void pageChatTouchDown(int16_t y, uint32_t ms);
bool pageChatScrollTouchMove(int16_t y, uint32_t ms, const NovaState& st);
void pageChatScrollTouchUp(uint32_t ms, const NovaState& st);
bool pageChatTick(uint32_t nowMs, const NovaState& st);
bool pageChatIsFlinging();
void pageChatScrollCancel();
void pageChatOnExit();
const char* pageChatQuickCmdText(int idx);

// 统一页面生命周期表（仿 Mooncake onEnter/onExit 与主题失效钩子）：
// UiNav::switchPage / applyTheme 经此表驱动，确保切页/切主题清理瞬态状态。
struct UiPageOps {
  UiPage page;
  void (*onEnter)(uint32_t nowMs);
  void (*onExit)();
  void (*onThemeChange)();
};

const UiPageOps* uiPageOps(UiPage p);

int uiPageRingIndex(UiPage p);

ui::Rect pageRankListRect();
ui::Rect pageTokensStageRect();
ui::Rect pageApprovalBarRect();

bool pageHomeHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageQuotaHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageRankHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageDetailHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageSettingsHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageApprovalHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
bool pageTokensHitRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out);
