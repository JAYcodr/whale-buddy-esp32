#pragma once
#include "model/settings.h"
#include "model/state.h"
#include "ui/canvas.h"
#include "ui/pages.h"
#include <stdint.h>

class UiNav {
public:
  void init(HomePageKind home);
  UiPage currentPage() const { return m_page; }
  void setPage(UiPage p);
  void goHome(HomePageKind home);

  UiAction onSwipe(int16_t dx, int16_t dy);
  UiAction onTap(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg);

  void draw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame, uint32_t nowMs);

private:
  UiPage m_page = UiPage::HOME;
};
