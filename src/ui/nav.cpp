#include "ui/nav.h"

namespace {
constexpr UiPage kPageRing[5] = {
  UiPage::TOKENS,
  UiPage::HOME,
  UiPage::QUOTA,
  UiPage::RANK,
  UiPage::DETAIL,
};

UiPage homeToPage(HomePageKind h) {
  switch (h) {
    case HomePageKind::QUOTA: return UiPage::QUOTA;
    case HomePageKind::RANK:  return UiPage::RANK;
    case HomePageKind::PET:
    default:                  return UiPage::HOME;
  }
}
}  // namespace

int uiPageRingIndex(UiPage p) {
  for (int i = 0; i < 5; i++) {
    if (kPageRing[i] == p) return i;
  }
  return 5;
}

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
  if (m_page == UiPage::SETTINGS) {
    // 设置页现在是可滚列表：垂直手势归列表，不能再拿来做「下滑退出」。
    // 返回走左上箭头 / Key1。
    return UiAction{};
  }
  int16_t adx = dx < 0 ? -dx : dx;
  int16_t ady = dy < 0 ? -dy : dy;
  if (adx < 48 || adx <= ady) return UiAction{};

  int idx = uiPageRingIndex(m_page);
  if (idx < 0 || idx > 4) idx = 1;
  if (dx < 0) {
    idx = (idx + 1) % 5;
  } else {
    idx = (idx + 4) % 5;
  }
  m_page = kPageRing[idx];
  return UiAction{ UiAction::GO_PAGE, (int)m_page };
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
  }
}
