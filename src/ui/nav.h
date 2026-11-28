#pragma once
#include "model/settings.h"
#include "model/state.h"
#include "ui/canvas.h"
#include "ui/pages.h"
#include <stdint.h>

class UiNav {
public:
  void init(HomePageKind home, uint32_t nowMs = 0);
  UiPage currentPage() const { return m_page; }
  void setPage(UiPage p);
  bool switchPage(UiPage next, uint32_t nowMs);
  void applyTheme(ui::UiTheme theme, uint32_t nowMs);
  void goHome(HomePageKind home, uint32_t nowMs = 0);

  UiAction onSwipe(int16_t dx, int16_t dy);
  UiAction onTap(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg);
  bool pressedRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out) const;

  void draw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame, uint32_t nowMs);

private:
  UiPage m_page = UiPage::HOME;
};
