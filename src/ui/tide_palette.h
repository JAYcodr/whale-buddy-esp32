#pragma once
#include "ui/theme.h"

// 深蓝潮汐（TIDE）主题的全部精确 RGB565 色值与共用绘制原语。
// 色值由 docs/design/tide-theme-task-card.md §2.2 定死，严禁擅自改动。
// 本文件只被各页面的 drawTide(...) 分支引用；OCEAN 路径不 include 它。
namespace ui::tide {

// ── 四大状态淹没主底色 ──────────────────────────────────────────────
constexpr uint16_t BG_COBALT   = rgb565(13,  71, 217);  // #0d47d9 工作 / 额度 / 设置 / 看板 / 榜单 / 在线心跳
constexpr uint16_t BG_NIGHT    = rgb565(11,  24,  48);  // #0b1830 摸鱼待机 / 语音录音底
constexpr uint16_t BG_TEAL     = rgb565(7,  102,  90);  // #07665a Token 鲸鱼页
constexpr uint16_t BG_CRIMSON  = rgb565(204, 31,  52);  // #cc1f34 审批拦截 / 断线告警 / 上滑取消 / 危险二次确认

// ── 叠层块色（全部由主底色混合得出，禁止出现黄/紫/霓虹杂色）─────────
constexpr uint16_t SURF_DEEP   = rgb565(10,  46, 144);  // 钴蓝底上的通透深靛蓝命令块（对齐设计稿 rgba(6,22,68,.52)）
constexpr uint16_t SURF_OPEN   = rgb565(10,  52, 158);  // 设置页展开行通透深蓝（对齐设计稿 rgba(6,22,68,.42)）
constexpr uint16_t SURF_SUB    = rgb565(34,  88, 224);  // 钴蓝底上的轻柔半透块（停着会话 / 进度条底轨）
constexpr uint16_t SURF_DARK   = rgb565(11,  12,  16);  // #0b0c10 审批页纯黑高对比命令盒
constexpr uint16_t SURF_INK    = rgb565(8,   28,  82);  // #081c52 白纸卡上的深蓝主文字 / 步骤窗底
constexpr uint16_t SURF_TINT   = rgb565(234, 240, 255); // #eaf0ff 白纸卡内的浅蓝信息条

// ── 文字与线条 ──────────────────────────────────────────────────────
constexpr uint16_t WHITE       = rgb565(255, 255, 255); // 纯白主字 / 实心反白底
constexpr uint16_t WHITE_DIM   = rgb565(196, 216, 255); // 钴蓝底上的次级说明字（对比度 > 6:1）
constexpr uint16_t WHITE_LINE  = rgb565(98,  140, 236); // 钴蓝底上的 1px 柔和分割线
constexpr uint16_t GIANT_WORK  = rgb565(64,  112, 232); // 钴蓝底上的 22% 纯白巨字时间色
constexpr uint16_t GIANT_IDLE  = rgb565(48,   64,  92); // 夜汐底上的 22% 纯白巨字时间色
constexpr uint16_t GIANT_ALERT = rgb565(222,  92, 108); // 绯红底上的 22% 纯白巨字时间色
constexpr uint16_t TEAL_LINE   = rgb565(76,  148, 138); // 深翠底上的 1px 分割线
constexpr uint16_t CRIM_LINE   = rgb565(226, 106, 120); // 绯红底上的 1px 分割线
constexpr uint16_t MUTED_ON_W  = rgb565(82,  104, 152); // 白纸卡上的次级说明字

// ── 按任意状态底色自适应派生次级字色 / 分割线（杜绝红底/翠底串蓝）───
inline uint16_t dimOn(uint16_t bg)  { return ui::blend565(bg, WHITE, 195); }
inline uint16_t lineOn(uint16_t bg) { return ui::blend565(bg, WHITE, 82); }

// ── 通用潮汐顶栏（高度 38px，零圆角胶囊，含防烧屏偏移）───────────────
// leftOverride 非空时替代左上角小时间（如主屏已有 140px 巨字时间，不再重复）。
inline void drawTopStrip(NovaCanvas& c, const NovaState& st,
                         int activeDot, uint16_t bgCol,
                         int totalDots = 6, const char* leftOverride = nullptr) {
  c.fillRect(0, 0, UI_W, 38, bgCol);
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);

  if (leftOverride && leftOverride[0]) {
    ui::drawText(c, ui::fontAscii14(), UI_SAFE_L + 2 + ox, 24 + oy,
                 leftOverride, dimOn(bgCol), bgCol);
  } else {
    char clk[8];
    snprintf(clk, sizeof(clk), "%02u:%02u", st.hour, st.minute);
    ui::drawText(c, ui::fontAscii14(), UI_SAFE_L + 2 + ox, 24 + oy, clk, WHITE, bgCol);
  }

  // 锐角方点页码指示器（0px 圆角，与原版胶囊区分）
  if (totalDots > 0 && activeDot >= 0) {
    constexpr int16_t actW = 16, dotW = 5, gap = 5;
    int16_t totW = (int16_t)((totalDots - 1) * (dotW + gap) + actW);
    int16_t px = (int16_t)(UI_W / 2 - totW / 2 + ox);
    int16_t py = (int16_t)(17 + oy);
    const uint16_t dimDot = ui::blend565(bgCol, WHITE, 90);
    for (int i = 0; i < totalDots; i++) {
      bool on = (i == activeDot);
      int16_t w = on ? actW : dotW;
      c.fillRect(px, py, w, 5, on ? WHITE : dimDot);
      px = (int16_t)(px + w + gap);
    }
  }

  char bat[12];
  snprintf(bat, sizeof(bat), "%u%%", (unsigned)st.battPct);
  ui::drawTextRight(c, ui::fontAscii14(), UI_SAFE_R - 2 + ox, 24 + oy, bat, WHITE, bgCol);
}

// ── 潮汐纯白大色块比例条（标签在条上方，绝不产生字色撞块）────────────
inline void drawBlockBar(NovaCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                         uint8_t pct, uint16_t bgCol) {
  if (pct > 100) pct = 100;
  uint16_t track = ui::blend565(bgCol, WHITE, 52);
  c.fillRect(x, y, w, h, track);
  int16_t fw = (int16_t)(((int32_t)w * pct + 50) / 100);
  if (fw > 0) c.fillRect(x, y, fw, h, WHITE);
}

// ── 设置页精密液位滑块（白描边 + 25%/50%/75% 微刻痕 + 两端读数）────────
inline void drawSliderBar(NovaCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                          uint8_t pct, uint16_t bgCol,
                          const char* loLbl, const char* hiLbl) {
  if (pct > 100) pct = 100;
  const uint16_t track = ui::blend565(bgCol, WHITE, 46);
  const uint16_t frame = ui::blend565(bgCol, WHITE, 155);
  c.fillRect(x, y, w, h, track);
  const int16_t fw = (int16_t)(((int32_t)w * pct + 50) / 100);
  if (fw > 0) c.fillRect(x, y, fw, h, WHITE);

  // 25% / 50% / 75% 精密刻痕，保证拉到 100% 也不像死白砖块
  for (int q = 1; q <= 3; q++) {
    const int16_t tx = (int16_t)(x + (int32_t)w * q / 4);
    const bool inFill = (tx - x < fw);
    const uint16_t tc = inFill ? ui::blend565(WHITE, bgCol, 90)
                               : ui::blend565(track, WHITE, 75);
    c.fillRect(tx, (int16_t)(y + h - 8), 1, 7, tc);
    c.fillRect(tx, (int16_t)(y + 1), 1, 5, tc);
  }
  c.drawRect(x, y, w, h, frame);

  const int16_t textBaseY = (int16_t)(y + h / 2 + 5);
  if (loLbl && loLbl[0]) {
    const bool onW = (fw > 38);
    ui::drawText(c, ui::fontAscii14(), (int16_t)(x + 8), textBaseY, loLbl,
                 onW ? bgCol : WHITE_DIM, onW ? WHITE : track);
  }
  if (hiLbl && hiLbl[0]) {
    const bool onW = (fw > w - 48);
    ui::drawTextRight(c, ui::fontAscii14(), (int16_t)(x + w - 8), textBaseY, hiLbl,
                      onW ? bgCol : WHITE_DIM, onW ? WHITE : track);
  }
}

}  // namespace ui::tide
