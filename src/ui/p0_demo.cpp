#include "ui/p0_demo.h"
#include <math.h>
#include <stdio.h>

namespace {
constexpr uint16_t COL_BG      = rgb565(8, 12, 22);
constexpr uint16_t COL_CARD    = rgb565(18, 25, 42);
constexpr uint16_t COL_BORDER  = rgb565(42, 62, 98);
constexpr uint16_t COL_TEXT    = rgb565(235, 242, 255);
constexpr uint16_t COL_DIM     = rgb565(132, 152, 186);
constexpr uint16_t COL_ACCENT  = rgb565(86, 184, 255);
constexpr uint16_t COL_MINT    = rgb565(84, 224, 176);
constexpr uint16_t COL_AMBER   = rgb565(255, 194, 82);

constexpr int ANIM_X = 24;
constexpr int ANIM_Y = 74;
constexpr int ANIM_W = 320;
constexpr int ANIM_H = 196;
}  // namespace

DirtyRect uiDrawP0AnimRegion(NovaCanvas& c, const P0DemoState& st) {
  // Background box for the 60fps dirty-rect region (simulating 128x155 pet + particles).
  c.fillRoundRect(ANIM_X, ANIM_Y, ANIM_W, ANIM_H, 18, COL_CARD);
  c.drawRoundRect(ANIM_X, ANIM_Y, ANIM_W, ANIM_H, 18, COL_BORDER);

  const float t = st.frame * 0.09f;
  const int cx = ANIM_X + ANIM_W / 2 + (int)(sinf(t * 0.7f) * 28.0f);
  const int cy = ANIM_Y + ANIM_H / 2 - 12 + (int)(cosf(t * 1.4f) * 8.0f);

  // Stylized whale-maid silhouette placeholder (128x155 footprint reference).
  c.drawRect(ANIM_X + (ANIM_W - 128) / 2, ANIM_Y + (ANIM_H - 155) / 2, 128, 155,
             rgb565(30, 44, 70));

  // Glow halo & breathing core
  c.fillCircle(cx, cy, 42, rgb565(24, 52, 92));
  c.fillCircle(cx, cy, 28, COL_ACCENT);
  c.fillCircle(cx - 8, cy - 8, 9, rgb565(215, 244, 255));

  // Orbiting water droplet particles
  for (int i = 0; i < 4; i++) {
    float a = t + i * 1.5708f;
    int px = cx + (int)(cosf(a) * 56.0f);
    int py = cy + (int)(sinf(a * 1.2f) * 30.0f);
    c.fillCircle(px, py, 4, (i & 1) ? COL_MINT : COL_ACCENT);
  }

  // Touch crosshair inside anim box if pressed
  if (st.touchDown) {
    c.drawCircle(st.touchX, st.touchY, 14, COL_AMBER);
    c.fillCircle(st.touchX, st.touchY, 4, COL_AMBER);
  }

  // Live telemetry inside anim card bottom
  c.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);
  c.setTextSize(2);
  c.setTextColor(COL_TEXT, COL_CARD);
  c.setCursor(ANIM_X + 16, ANIM_Y + ANIM_H - 14);
  c.printf("%.1f fps · push %.1f ms", st.fps, st.pushUs / 1000.0f);
  c.setFont((const GFXfont*)nullptr);

  return DirtyRect{ ANIM_X, ANIM_Y, ANIM_W, ANIM_H };
}

void uiDrawP0Full(NovaCanvas& c, const P0DemoState& st) {
  c.fillScreen(COL_BG);

  // Safe inset frame marker (calibrated for rounded AMOLED bezel)
  c.drawRoundRect(UI_SAFE_L, UI_SAFE_T, UI_SAFE_W, UI_SAFE_H, 22, rgb565(24, 34, 56));

  // Top status bar
  c.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);
  c.setTextSize(2);
  c.setTextColor(COL_TEXT, COL_BG);
  c.setCursor(UI_SAFE_L + 8, UI_SAFE_T + 26);
  c.printf("%02u:%02u:%02u", st.hour, st.minute, st.second);

  c.setTextColor(st.usbPresent ? COL_MINT : COL_AMBER, COL_BG);
  c.setCursor(UI_SAFE_R - 116, UI_SAFE_T + 26);
  c.printf("%u%% %s", st.battPct, st.usbPresent ? "USB" : "BAT");

  // Middle animated region
  uiDrawP0AnimRegion(c, st);

  // Bottom verification card (CJK + typography + memory)
  const int cardY = 284;
  const int cardH = 136;
  c.fillRoundRect(ANIM_X, cardY, ANIM_W, cardH, 16, COL_CARD);
  c.drawRoundRect(ANIM_X, cardY, ANIM_W, cardH, 16, COL_BORDER);

  c.setFont((const uint8_t*)u8g2_font_cubic11_h_cjk);
  c.setTextSize(2);
  c.setTextColor(COL_ACCENT, COL_CARD);
  c.setCursor(ANIM_X + 16, cardY + 38);
  c.print("诺瓦副屏 P0 · 368x448");

  c.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);
  c.setTextSize(2);
  c.setTextColor(COL_TEXT, COL_CARD);
  c.setCursor(ANIM_X + 16, cardY + 72);
  c.print("中文渲染 / 触控 / 局部脏矩形正常");

  c.setTextColor(COL_DIM, COL_CARD);
  c.setCursor(ANIM_X + 16, cardY + 102);
  c.printf("Heap %luK · PSRAM %luK",
           (unsigned long)(st.freeHeap / 1024),
           (unsigned long)(st.freePsram / 1024));

  c.setCursor(ANIM_X + 16, cardY + 124);
  if (st.touchDown) {
    c.setTextColor(COL_AMBER, COL_CARD);
    c.printf("Touch (%d, %d)", st.touchX, st.touchY);
  } else {
    c.printf("轻触屏幕测试音效 · Key1 全屏重绘");
  }
  c.setFont((const GFXfont*)nullptr);
}
