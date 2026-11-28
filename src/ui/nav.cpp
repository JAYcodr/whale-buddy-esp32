#include "ui/nav.h"

namespace {
constexpr UiPage kPageRing[6] = {
  UiPage::TOKENS,
  UiPage::HOME,
  UiPage::QUOTA,
  UiPage::RANK,
  UiPage::DETAIL,
  UiPage::CHAT,
};

UiPage homeToPage(HomePageKind h) {
  switch (h) {
    case HomePageKind::QUOTA: return UiPage::QUOTA;
    case HomePageKind::RANK:  return UiPage::RANK;
    case HomePageKind::PET:
    default:                  return UiPage::HOME;
  }
}
void settingsEnterHook(uint32_t) { pageSettingsOnEnter(); }

constexpr UiPageOps kPageRegistry[] = {
  { UiPage::HOME,     pageHomeOnEnter,   pageHomeOnExit,     pageHomeOnThemeChange },
  { UiPage::QUOTA,    nullptr,           pageQuotaOnExit,    nullptr },
  { UiPage::RANK,     nullptr,           pageRankOnExit,     nullptr },
  { UiPage::DETAIL,   nullptr,           nullptr,            nullptr },
  { UiPage::SETTINGS, settingsEnterHook, pageSettingsOnExit, nullptr },
  { UiPage::TOKENS,   pageTokensOnEnter, pageTokensOnExit,   pageTokensOnThemeChange },
  { UiPage::CHAT,     nullptr,           pageChatOnExit,     nullptr },
};

}  // namespace

const UiPageOps* uiPageOps(UiPage p) {
  for (const UiPageOps& ops : kPageRegistry) {
    if (ops.page == p) return &ops;
  }
  return nullptr;
}

int uiPageRingIndex(UiPage p) {
  for (int i = 0; i < 6; i++) {
    if (kPageRing[i] == p) return i;
  }
  return 6;
}

void UiNav::init(HomePageKind home, uint32_t nowMs) {
  m_page = homeToPage(home);
  const UiPageOps* ops = uiPageOps(m_page);
  if (ops && ops->onEnter) ops->onEnter(nowMs);
}

void UiNav::setPage(UiPage p) {
  m_page = p;
}

bool UiNav::switchPage(UiPage next, uint32_t nowMs) {
  if (next == m_page) return false;
  const UiPageOps* oldOps = uiPageOps(m_page);
  if (oldOps && oldOps->onExit) oldOps->onExit();
  m_page = next;
  const UiPageOps* newOps = uiPageOps(m_page);
  if (newOps && newOps->onEnter) newOps->onEnter(nowMs);
  return true;
}

void UiNav::applyTheme(ui::UiTheme theme, uint32_t nowMs) {
  (void)nowMs;
  ui::setActiveTheme(theme);
  for (const UiPageOps& ops : kPageRegistry) {
    if (ops.onThemeChange) ops.onThemeChange();
  }
}

void UiNav::goHome(HomePageKind home, uint32_t nowMs) {
  switchPage(homeToPage(home), nowMs);
}

UiAction UiNav::onSwipe(int16_t dx, int16_t dy) {
  if (m_page == UiPage::SETTINGS) {
    // 设置页现在是可滚列表：垂直手势归列表，不能再拿来做「下滑退出」。
    // 返回走左上箭头 / Key1。
    return UiAction{};
  }
  int16_t adx = dx < 0 ? -dx : dx;
  int16_t ady = dy < 0 ? -dy : dy;
  if (adx < 48 || adx <= ady) return UiAction{};

  int idx = uiPageRingIndex(m_page);
  if (idx < 0 || idx > 5) idx = 1;
  if (dx < 0) {
    idx = (idx + 1) % 6;
  } else {
    idx = (idx + 5) % 6;
  }
  // 不在这里直接改 m_page，交给 switchPage() 触发 old.onExit -> new.onEnter。
  return UiAction{ UiAction::GO_PAGE, (int)kPageRing[idx] };
}

UiAction UiNav::onTap(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg) {
  if (st.approvalActive) {
    return pageApprovalTap(x, y);
  }
  switch (m_page) {
    case UiPage::HOME:     return pageHomeTap(x, y, st);
    case UiPage::QUOTA:    return pageQuotaTap(x, y, st);
    case UiPage::RANK:     return pageRankTap(x, y);
    case UiPage::DETAIL:   return pageDetailTap(x, y);
    case UiPage::SETTINGS: return pageSettingsTap(x, y, cfg);
    case UiPage::TOKENS:   return pageTokensTap(x, y, st, st.nowMs);
    case UiPage::CHAT:     return pageChatTap(x, y, st);
    default:               return UiAction{};
  }
}

bool UiNav::pressedRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg,
                        ui::Rect* out) const {
  if (!out) return false;
  if (st.approvalActive) {
    return pageApprovalHitRect(x, y, st, cfg, out);
  }
  switch (m_page) {
    case UiPage::HOME:     return pageHomeHitRect(x, y, st, cfg, out);
    case UiPage::QUOTA:    return pageQuotaHitRect(x, y, st, cfg, out);
    case UiPage::RANK:     return pageRankHitRect(x, y, st, cfg, out);
    case UiPage::DETAIL:   return pageDetailHitRect(x, y, st, cfg, out);
    case UiPage::SETTINGS: return pageSettingsHitRect(x, y, st, cfg, out);
    case UiPage::TOKENS:   return pageTokensHitRect(x, y, st, cfg, out);
    case UiPage::CHAT:     return pageChatHitRect(x, y, st, cfg, out);
    default:               return false;
  }
}

void UiNav::draw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg,
                 uint32_t frame, uint32_t nowMs) {
  if (st.approvalActive) {
    pageApprovalDraw(c, st, nowMs);
    return;
  }
  switch (m_page) {
    case UiPage::HOME:     pageHomeDraw(c, st, cfg, frame); break;
    case UiPage::QUOTA:    pageQuotaDraw(c, st); break;
    case UiPage::RANK:     pageRankDraw(c, st); break;
    case UiPage::DETAIL:   pageDetailDraw(c, st, cfg); break;
    case UiPage::SETTINGS: pageSettingsDraw(c, st, cfg); break;
    case UiPage::TOKENS:   pageTokensDraw(c, st, nowMs); break;
    case UiPage::CHAT:     pageChatDraw(c, st); break;
  }
}
