#include "ui/nav.h"

namespace {
UiPage homeToPage(HomePageKind h) {
  switch (h) {
    case HomePageKind::QUOTA: return UiPage::QUOTA;
    case HomePageKind::RANK:  return UiPage::RANK;
    case HomePageKind::PET:
    default:                  return UiPage::HOME;
  }
}
}  // namespace

void UiNav::init(HomePageKind home) {
  m_page = homeToPage(home);
}

void UiNav::setPage(UiPage p) {
  m_page = p;
}

void UiNav::goHome(HomePageKind home) {
  m_page = homeToPage(home);
}

UiAction UiNav::onSwipe(int16_t dx, int16_t dy) {
  if (m_page == UiPage::SETTINGS) return UiAction{};
  int16_t adx = dx < 0 ? -dx : dx;
  int16_t ady = dy < 0 ? -dy : dy;
  if (adx < 48 || adx <= ady) return UiAction{};

  int idx = (int)m_page;
  if (idx < 0 || idx > 3) idx = 0;
  if (dx < 0) {
    idx = (idx + 1) % 4;
  } else {
    idx = (idx + 3) % 4;
  }
  m_page = (UiPage)idx;
  return UiAction{ UiAction::GO_PAGE, idx };
}

UiAction UiNav::onTap(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg) {
  if (st.approvalActive) {
    return pageApprovalTap(x, y);
  }
  switch (m_page) {
    case UiPage::HOME:     return pageHomeTap(x, y, st);
    case UiPage::QUOTA:    return pageQuotaTap(x, y);
    case UiPage::RANK:     return pageRankTap(x, y);
    case UiPage::DETAIL:   return pageDetailTap(x, y);
    case UiPage::SETTINGS: return pageSettingsTap(x, y, cfg);
    default:               return UiAction{};
  }
}

void UiNav::draw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg,
                 uint32_t frame, uint32_t nowMs) {
  if (st.approvalActive) {
    pageApprovalDraw(c, st, nowMs);
    return;
  }
  switch (m_page) {
    case UiPage::HOME:     pageHomeDraw(c, st, frame); break;
    case UiPage::QUOTA:    pageQuotaDraw(c, st); break;
    case UiPage::RANK:     pageRankDraw(c, st); break;
    case UiPage::DETAIL:   pageDetailDraw(c, st, cfg); break;
    case UiPage::SETTINGS: pageSettingsDraw(c, cfg); break;
  }
}
