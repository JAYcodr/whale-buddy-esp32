# 任务卡：Nova Buddy「深海原版 ⇄ 深蓝潮汐」可切换双主题实现

> **角色分工**：本任务卡由 **Carbon（架构师 / 艺术总监）** 定死视觉契约、接口规范与标准样板；由 **DeepSeek V4.1 Flash (Max)** 严格按契约完成代码实现与自动化回归；完成后由 **Carbon 终审** `tools/sim/out/tide_*.png` 像素图与代码 Diff。
>
> **视觉定稿参照（1:1 真机比例 HTML 小样）**：
> - 主屏 / Token / 额度 / 审批：`docs/design/style-concepts.html`（方案 `04 深蓝潮汐`）
> - 设置页 / 命令看板页：`docs/design/tide-settings-board.html`
> - 编程榜 / 设备心跳 / AOD：`docs/design/tide-rank-detail.html`

---

## 0. 开工前先拴四根绳（直接复制这段发给 DeepSeek）

```text
请先阅读仓库里的 docs/design/tide-theme-task-card.md 任务卡，以及它列出的几个契约锚点文件（先抽样读，别全仓乱扫）。

先说清楚不用管什么：
1. 不要自创任何配色、圆角、阴影或排版，所有 RGB565 色值、字库、坐标严格照任务卡写死的值执行。
2. 不要改动「深海原版（OCEAN）」的任何绘制像素，原有 51 项 tools/sim 基线必须 100% 零改动通过。
3. 不要改动 AOD 熄屏时钟（保持纯黑省电防烧屏），不要改动现有 page*Draw(...) 的对外函数签名。
4. 不要碰 host-plugin/、src/hw/、src/net/，也先不要改 .nva 二进制格式（Phase 1 直接复用现有 .nva）。
5. src/ 下严禁写裸数字 368（check_all.sh 有硬门禁），统一用 UI_W / UI_SAFE_L / UI_SAFE_R / UI_SAFE_W。

顺口亮底牌：你负责严格按契约和样板文件把 7 个页面 + NVS 主题开关 + tools/sim 潮汐渲染用例写完并跑通 check_all.sh 和固件编译；做完后我会交给 Carbon 逐张看 tools/sim/out/tide_*.png 做视觉与代码终审，所以不要自由发挥。

现在先别动手改代码——用三句话复述你的目标、负向边界和分步计划，我确认后你再开工。
```

---

## 1. 架构契约与目录变更地图

### 1.1 不改函数签名的核心约定
当前 `pageQuotaDraw(c, st)` / `pageRankDraw(c, st)` / `pageApprovalDraw(c, st, nowMs)` / `pageChatDraw(c, st)` 均不接收 `cfg`。为保证零破坏性迁移：
- **禁止修改** 任何 `page*Draw` / `page*Tap` / `page*HitRect` 的函数签名；
- 在 `src/ui/theme.h` / `src/ui/theme.cpp` 提供全局轻状态 `ui::activeTheme()` / `ui::setActiveTheme(UiTheme)` / `ui::isTideTheme()`；
- `src/main.cpp` 在开机 `settingsLoad()` 后、每次 `SET_THEME` 后、每次 `RESTORE_DEFAULTS` 后调用 `ui::setActiveTheme(cfg.uiTheme)`；
- **AOD 规则**：`pageHomeDrawAod()` 始终绘制纯黑 `#000000` 表盘，不进入潮汐分支。

### 1.2 允许改动的文件清单（仅限以下范围）

| 文件 | 操作 | 职责边界 |
|---|---|---|
| `src/ui/tide_palette.h` | **新增** | 潮汐主题全部精确 RGB565 色值与共用绘制原语（见 §2） |
| `src/ui/theme.h` / `src/ui/theme.cpp` | 修改 | 新增 `UiTheme` 枚举、`activeTheme()` / `setActiveTheme()` / `isTideTheme()` |
| `src/model/settings.h` | 修改 | 新增 `uint8_t uiTheme = 0;`（NVS key `"theme"`, choices `{0, 1}`, `toHost=false`） |
| `src/ui/pages.h` / `src/ui/settings_action.cpp` | 修改 | 新增 `UiAction::SET_THEME` 并映射到 `kSettings` 下标 `14` |
| `src/main.cpp` | 修改 | 同步 `ui::setActiveTheme((ui::UiTheme)cfg.uiTheme)`；处理 `SET_THEME` 触发整屏重绘 |
| `src/ui/page_approval.cpp` | 修改 | **按 §3 标准样板文件逐行实现** |
| `src/ui/page_home.cpp` | 修改 | 潮汐主屏（状态淹没 + `fontNum92()` 巨字时间背景 + 诺瓦前景 + 局部舞台重绘） |
| `src/ui/page_tokens.cpp` | 修改 | 潮汐 Token 页（深翠淹没 + 巨字读数 + 纯白鲸鱼 + 局部舞台清底改深翠） |
| `src/ui/page_quota.cpp` | 修改 | 潮汐额度页（钴蓝底 + 顶部巨字余额 + 纯白大色块比例条，标签在条上方零撞色） |
| `src/ui/page_chat.cpp` | 修改 | 潮汐命令看板页（几何命令方块 + 待批红块 + 反白展开 + 夜汐录音 + 白纸确认回执） |
| `src/ui/page_settings.cpp` | 修改 | 新增首位 `R_THEME` 行（两个主题都可见！）；潮汐版通栏去卡片 + 38px 液位滑块 + 红屏二次确认 |
| `src/ui/page_rank.cpp` | 修改 | 潮汐编程榜（钴蓝通栏记分牌 + 活跃模型整行反白 + 列表清底改钴蓝） |
| `src/ui/page_detail.cpp` | 修改 | 潮汐设备心跳（在线钴蓝 / 断线绯红 + 顶部 `92%` & `60fps` 巨数双列头） |
| `tools/sim/main.cpp` | 修改 | 新增 15 个 `tide_*.png` 独立渲染输出（仅保存 PNG 供 Carbon 验图；原版 51 项基线中仅允许因新增 `R_THEME` 影响的 `page_settings*` 项同步更新并附 `git diff baseline.txt`） |
| `tools/test_settings.sh`（及其测试 C++） | 按需同步 | 若断言了 `kSettings` 数量或恢复默认字段，同步补上 `theme` 默认值 `0` 的断言 |

> **注意**：`page_settings.cpp` 的 `R_THEME`（「界面风格」）项在 **深海原版** 和 **深蓝潮汐** 两种主题下都必须显示，否则切进原版就切不回来了！如果 `R_THEME` 影响了原版 `settings` 模拟器基线图的布局，请把 `R_THEME` 放在「屏幕」分组末尾或先确认 `tools/sim/check.sh` 哪些 `page_settings*` 基线会变化并如实汇报给 Carbon，不要私自覆盖 `baseline.txt`！

---

## 2. 接口契约（直接照抄）

### 2.1 `src/ui/theme.h` 新增片段
```cpp
enum class UiTheme : uint8_t {
  OCEAN = 0,  // 深海原版（默认，保持原有 51 项基线零变更）
  TIDE  = 1,  // 深蓝潮汐（仅亮屏启用，AOD 恒回纯黑）
};

UiTheme activeTheme();
void    setActiveTheme(UiTheme t);
inline bool isTideTheme() { return activeTheme() == UiTheme::TIDE; }
```
在 `src/ui/theme.cpp` 中：
```cpp
static UiTheme s_activeTheme = UiTheme::OCEAN;
UiTheme activeTheme() { return s_activeTheme; }
void setActiveTheme(UiTheme t) { s_activeTheme = t; }
```

### 2.2 `src/ui/tide_palette.h` 完整契约（严禁擅自改色值）
```cpp
#pragma once
#include "ui/theme.h"

namespace ui::tide {

// ── 四大状态淹没主底色 ──────────────────────────────────────────────
constexpr uint16_t BG_COBALT   = rgb565(13,  71, 217);  // #0d47d9 工作 / 额度 / 设置 / 看板 / 榜单 / 在线心跳
constexpr uint16_t BG_NIGHT    = rgb565(11,  24,  48);  // #0b1830 摸鱼待机 / 语音录音底
constexpr uint16_t BG_TEAL     = rgb565(7,  102,  90);  // #07665a Token 鲸鱼页
constexpr uint16_t BG_CRIMSON  = rgb565(204, 31,  52);  // #cc1f34 审批拦截 / 断线告警 / 上滑取消 / 危险二次确认

// ── 叠层块色（全部由主底色混合得出，禁止出现黄/紫/霓虹杂色）─────────
constexpr uint16_t SURF_DEEP   = rgb565(8,   34, 108);  // 钴蓝底上的深靛蓝命令块 / 展开面板
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

// ── 通用潮汐顶栏（高度 38px，零圆角胶囊，含防烧屏偏移）───────────────
inline void drawTopStrip(NovaCanvas& c, const NovaState& st,
                         int activeDot, uint16_t bgCol, int totalDots = 6) {
  c.fillRect(0, 0, UI_W, 38, bgCol);
  const int16_t ox = ui::burnOffsetX(st.burnInStep);
  const int16_t oy = ui::burnOffsetY(st.burnInStep);

  char clk[8];
  snprintf(clk, sizeof(clk), "%02u:%02u", st.hour, st.minute);
  ui::drawText(c, ui::fontAscii14(), UI_SAFE_L + 2 + ox, 24 + oy, clk, WHITE, bgCol);

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
  uint16_t track = ui::blend565(bgCol, WHITE, 48);
  c.fillRect(x, y, w, h, track);
  int16_t fw = (int16_t)(((int32_t)w * pct + 50) / 100);
  if (fw > 0) c.fillRect(x, y, fw, h, WHITE);
}

}  // namespace ui::tide
```

---

## 3. 标准样板文件：`src/ui/page_approval.cpp`（完整参考实现）

其余 6 个页面全部照这个模式写：**原版函数不动一像素；潮汐分支独立成 `drawTide(...)`；绘制与点击热区共用矩形 helper；局部刷新函数同样走 `isTideTheme()` 分支防黑块！**

```cpp
// 在 page_approval.cpp 顶部新增：
#include "ui/tide_palette.h"

// 按钮矩形改为主题感知 helper（Draw / Tap / HitRect 三处共用！）
inline ui::Rect btnDenyRect() {
  return ui::isTideTheme() ? ui::Rect{ 18, 366, 184, 54 }
                           : ui::Rect{ 36, 302, 140, 52 };
}
inline ui::Rect btnAllowRect() {
  return ui::isTideTheme() ? ui::Rect{ 212, 366, 138, 54 }
                           : ui::Rect{ 192, 302, 140, 52 };
}
inline ui::Rect btnToPcRect() {
  return ui::isTideTheme() ? ui::Rect{ 18, 318, 332, 38 }
                           : ui::Rect{ 36, 366, 296, 44 };
}
inline ui::Rect barRectActive() {
  return ui::isTideTheme() ? ui::Rect{ 0, 0, UI_W, 5 }
                           : ui::Rect{ kBarX, kBarY, kBarW, kBarH };
}

// 局部刷新倒计时条（防黑块关键示范！）
void pageApprovalDrawBar(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  ApprovalTiming t = computeTiming(st, nowMs);
  if (ui::isTideTheme()) {
    const uint16_t track = ui::blend565(ui::tide::BG_CRIMSON, ui::tide::WHITE, 64);
    c.fillRect(0, 0, UI_W, 5, track);
    int16_t fw = (int16_t)(((uint64_t)UI_W * t.rem + t.ttl / 2) / t.ttl);
    if (fw > 0) c.fillRect(0, 0, fw, 5, ui::tide::WHITE);
    return;
  }
  // 原版 OCEAN 代码保持一字不改 ...
}

// 潮汐版整页绘制
void drawApprovalTide(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  using namespace ui::tide;
  c.fillScreen(BG_CRIMSON);
  pageApprovalDrawBar(c, st, nowMs);

  // 顶行提示
  ui::drawText(c, ui::fontAscii14(), 18, 30, "STOP // INTERCEPT", WHITE, BG_CRIMSON);
  ui::drawTextRight(c, ui::fontCjk14(), UI_W - 18, 30, "超时自动拒", WHITE, BG_CRIMSON);

  // 超大倒计时数字（复用已有 fontNum46()）+ 右侧简短人话/工具名
  ApprovalTiming t = computeTiming(st, nowMs);
  char secStr[16];
  snprintf(secStr, sizeof(secStr), "%us", (unsigned)((t.rem + 999) / 1000));
  ui::drawText(c, ui::fontNum46(), 18, 92, secStr, WHITE, BG_CRIMSON);

  char toolLine[40];
  snprintf(toolLine, sizeof(toolLine), "工具 · %s", st.apTool[0] ? st.apTool : "Tool");
  ui::drawTextRight(c, ui::fontCjk16(), UI_W - 18, 76, toolLine, WHITE, BG_CRIMSON);
  ui::drawTextRight(c, ui::fontCjk14(), UI_W - 18, 98, "危险操作待确认",
                    ui::blend565(BG_CRIMSON, WHITE, 200), BG_CRIMSON);

  // 中部纯黑高对比命令盒（0px 圆角，锐利边缘）
  c.fillRect(18, 118, UI_W - 36, 188, SURF_DARK);
  ui::drawText(c, ui::fontAscii14(), 32, 144, "TOOL // REASON", rgb565(255, 112, 128), SURF_DARK);
  const char* reason = st.apReason[0] ? st.apReason : "DSH 请求执行该操作";
  ui::drawWrappedUtf8(c, ui::fontCjk16(), 32, 174, UI_W - 64, 28, 4, reason, WHITE, SURF_DARK);

  // 次要操作：转电脑端处理（细白框）
  const ui::Rect rPc = btnToPcRect();
  c.drawRect(rPc.x, rPc.y, rPc.w, rPc.h, CRIM_LINE);
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), rPc, "转电脑端处理", WHITE, BG_CRIMSON);

  // 主操作：左纯白实心「拒绝拦截」（面积更大最防误触），右 2px 白描边「批准执行」
  const ui::Rect rDeny = btnDenyRect();
  c.fillRect(rDeny.x, rDeny.y, rDeny.w, rDeny.h, WHITE);
  ui::drawTextCenteredInRect(c, ui::fontCjk16(), rDeny, "拒绝拦截", BG_CRIMSON, WHITE);

  const ui::Rect rAllow = btnAllowRect();
  c.drawRect(rAllow.x, rAllow.y, rAllow.w, rAllow.h, WHITE);
  c.drawRect(rAllow.x + 1, rAllow.y + 1, rAllow.w - 2, rAllow.h - 2, WHITE);
  ui::drawTextCenteredInRect(c, ui::fontCjk16(), rAllow, "批准执行", WHITE, BG_CRIMSON);
}
```
> 在 `pageApprovalDraw()` 开头加 `if (ui::isTideTheme()) { drawApprovalTide(c, st, nowMs); return; }`；`pageApprovalTap()` / `pageApprovalHitRect()` / `pageApprovalBarRect()` 全部改调 `btnDenyRect()` / `btnAllowRect()` / `btnToPcRect()` / `barRectActive()`。

---

## 4. 其余 6 个页面精确实现规格（逐页对照 HTML 小样）

### ① 主屏 `page_home.cpp`（状态淹没 + 巨字叠压）
- **底色映射**：
  - `st.approvalActive || st.turnError || st.wait > 0` → `BG_CRIMSON`（巨字色 `GIANT_ALERT`）
  - `st.run > 0` → `BG_COBALT`（巨字色 `GIANT_WORK`）
  - 其余（空闲/摸鱼）→ `BG_NIGHT`（巨字色 `GIANT_IDLE`）
- **顶栏**：`drawTopStrip(c, st, 1, bg)`（高度 38px），左时间/中页点/右电量。
- **舞台层（`y = 38 .. 332`）**：
  - **巨字时间背景层**：用现有 `fontNum92()` 居中画出 `HH:MM`（`baselineY = 148 + oy`，颜色 `giantCol`），位于诺瓦背后；
  - 状态眉标：`y = 172` 左对齐 `ACTIVE · 运行中` / `STANDBY · 摸鱼中` / `ALERT · 异常`（`fontCjk14`, `WHITE_DIM`）；
  - 前景诺瓦：`nvaDraw(...)` 直接叠画在巨字前景；
  - 气泡（有 `s_bubbleBuf` 时）：纯白实心 0–4px 锐角小块 + `SURF_INK` 文字，置于右上 `y = 160` 不遮脸；
  - **局部刷新防黑块（必做！）**：`pageHomeDrawStage()` 在潮汐模式下必须用当前状态 `bg` 清矩形，并且若脏矩形与 `fontNum92()` 区域相交，先重画巨字时间再 `nvaDraw`，绝不能清成 `COL_BG` 黑色！
- **底栏码头（`y = 342 .. 432`）**：顶部 `2px` 细横线（`blend565(bg, WHITE, 95)`）；左侧纯白实心反色块 `RUN N · CTX XX%`（文字用 `bg`），右侧模型名；下方一行 `fontCjk16()` 纯白单行指令/状态文本（`st.msg` 或 `st.sessTitle`）。
- **AP 配网态 & AOD 态**：保持原版不动（AP 二维码黑字白底，AOD 强制纯黑省电）。

### ② Token 鲸鱼页 `page_tokens.cpp`（深翠海青淹没）
- **整页底色**：`BG_TEAL`。
- **顶栏**：`drawTopStrip(c, st, 0, BG_TEAL)`。
- **顶部巨数区（`y = 44 .. 138`）**：左侧用 `fontNum46()` 纯白大字显示当日/选中日数值（如 `6.61`），旁接 `亿`（`fontCjk16()`）；右上角放纯黑实心小标 `PEAK`（选中峰值日时）或日期 `11/23`。
- **鲸鱼舞台（`pageTokensDrawStage` 必同步清底为 `BG_TEAL`！）**：
  - 常规小鲸鱼：`blend565(BG_TEAL, WHITE, 210)`，眼睛 `BG_TEAL`；
  - 选中/当日小鲸鱼：`SURF_DARK` 深黑填充 + 纯白眼睛（与深翠背景形成极致反差）；
- **底栏（`y = 356 .. 432`）**：上方 `1px` `TEAL_LINE` 横线，两栏大字并排：`日均 X.XX亿` | `缓存 XX.X%`（无圆角卡片）。

### ③ 钱包额度页 `page_quota.cpp`（钴海蓝 + 纯白比例大块）
- **整页底色**：`BG_COBALT`。
- **顶栏**：`drawTopStrip(c, st, 2, BG_COBALT)`。
- **余额 Hero 区（`y = 44 .. 148`）**：如果第 0 卡是 `bal`（DeepSeek），直接去卡片，顶部小字 `DEEPSEEK BALANCE` + 右侧刷新时间，下方 `fontNum46()` 纯白大字 `¥77.10`（余额低于 `kBalCrit` 时整页或本块翻 `BG_CRIMSON`），底一条 `2px` `WHITE_LINE`。
- **周期额度区（`y = 160 .. 426`）**：逐条渲染各 `win` 卡的 `bars`：
  - 上行：`fontCjk14()` 左对齐「`OpenCode 周限`」、右对齐 `73% · 1h59m`（纯白）；
  - 下行：`ui::tide::drawBlockBar(c, 18, y, UI_W - 36, 20, pct, BG_COBALT)`（**标签永远在条上方，禁止把字压在白块内部避免撞色！**）；
  - `kind == "na"` 空态卡：用 `SURF_DEEP` 实心矩形条 + 纯白说明字，点击仍触发 `SYNC_HOST`。

### ④ 编程榜 `page_rank.cpp`（钴海蓝记分牌）
- **整页底色**：`BG_COBALT`（**注意 `drawRankRowsAndScrollbar()` 和 `drawRankHeaderAndFooter()` 清底色全部改 `BG_COBALT`！**）。
- **顶栏与表头**：`drawTopStrip(c, st, 3, BG_COBALT)`；`y = 44..88` 左 `编程榜 1-5/24`（`fontCjk16` 纯白），右纯白实心反色模式切换钮 `B·F·K 维度` / `审美·耗时`（钴蓝字，复用 `BTN_MODE` 热区同步）；下方 `2px` 纯白实线。
- **榜单行（去圆角卡片，通栏平铺 `x=0..UI_W-8`，行间 `1px` `WHITE_LINE`）**：
  - 第 1 名：左侧 `5px` 纯白竖标；
  - 活跃模型（`active == true`）：**整行铺满 `WHITE` 纯白底，全部文字改 `SURF_INK` 深蓝**；
  - 排名数字用 `fontNum24()`，总分右对齐 `fontNum24()`，副行指标全纯白/浅蓝等宽（去掉五颜六色杂色）；
  - 右侧滚动条：底轨 `blend565(BG_COBALT, WHITE, 50)`，滑块纯白 `WHITE`。

### ⑤ 设备心跳 `page_detail.cpp`（在线钴蓝 / 断线信号红）
- **状态底色**：`bool off = (!st.wifiConnected || !st.mqttConnected);` → `off ? BG_CRIMSON : BG_COBALT`。
- **顶部双列巨数遥测头（`y = 42 .. 152`）**：
  - 左列：小字 `BATTERY` + `fontNum46()` 巨字 `92%`（断线时显示 `OFF`）+ 下行电压/电流；
  - 右列（左 1px 竖线隔开）：小字 `RENDER` + `fontNum46()` 巨字 `60` + `fps` + 耗时/主频；
  - 底部 `2px` 纯白横线。
- **中部 5 行纯白细线清单（`y = 160 .. 346`，无卡片）**：设备标识 / WiFi / MQTT（带纯白反色状态章 `在线` 或 `断开`）/ 当前模型 / 主题策略（`潮汐亮屏 · AOD 回纯黑`）。
- **底部主按钮**：复用 `BTN_SETTINGS` 热区，画成纯白实心 0px 锐角大钮（钴蓝/绯红粗字），底行保留 Key1 提示。

### ⑥ 设置页 `page_settings.cpp`（去卡片通栏 + 38px 液位滑块）
- **新增设置项 `R_THEME`（「界面风格」）**：置于 `R_GRP_SCR` 分组内，选项 `{ "深海原版", "深蓝潮汐" }`（值 `{ 0, 1 }`），触发 `UiAction::SET_THEME`。
- **潮汐样式（`pageSettingsDraw` 和 `pageSettingsDrawList` 清底均改 `BG_COBALT`！）**：
  - 顶栏 58px：`‹ 设置` + 右侧纯白反色小章 `潮汐` + 底 `2px` 纯白横线；
  - 去圆角外框：分组头左侧 `SURF_DEEP` 编号块（`01`~`06`），行间 `1px` `WHITE_LINE`，展开行底色 `SURF_DEEP`；
  - 选项按钮：未选 `1.5px` 白框半透，选中**纯白实心反色**；
  - 滑块（亮度/音量）：38px 高 `drawBlockBar(...)` 实心液位条（直接复用现有触摸坐标换算，扩大垂直命中区更好拖）；
  - 开关：锐利双格 `开 | 关` 白框反色拨片；
  - 危险二次确认行（`s_confirmRow == idx`）：整行背景切换为 `BG_CRIMSON` 信号绯红。

### ⑦ 命令看板页 `page_chat.cpp`（几何命令方块 + 全 4 态）
- **巡检 / 展开 / 确认 / 回执底色**：`BG_COBALT`；**录音态底色**：`BG_NIGHT`（上滑取消 `st.boardHoldCancel` 时切 `BG_CRIMSON`！）。
- **巡检态会话方块**：
  - `RUN`：`SURF_DEEP` 实心块 + 左侧 `5px` 纯白竖标；
  - `WAIT`：**整块铺满 `BG_CRIMSON` 信号绯红** + 左侧 `5px` 白竖标；
  - `IDLE`：`SURF_SUB` 轻块；
- **展开态**：展开卡反白为 `WHITE` 纯白底 + `SURF_INK` 深蓝字，步骤区嵌 `SURF_INK` 终端黑蓝盒；其余折叠成 `MINI_CARD_H` 极简条；
- **底部按住说话大钮（`dockBtnRect`）**：纯白实心通栏大块 + 钴蓝粗字 `按住说话`；
- **识别确认卡 & 单卡回执**：纯白大卡 `WHITE` + `SURF_INK` 大字，按钮区：`重说`（深蓝细框）/ `发送`或`排队`（`BG_COBALT` 实心）/ `打断`（`BG_CRIMSON` 实心）。

---

## 5. 分阶段执行与硬性验收门

### 执行顺序（建议每完成一个阶段跑一次 `bash tools/sim/check.sh`）
1. **阶段 A（骨架 + 样板）**：落 `tide_palette.h`、`theme.h/.cpp`、`settings.h`、`settings_action.cpp`、`main.cpp` 同步点 + `page_approval.cpp` 标准样板；跑 `bash tools/test_settings.sh` 与 `bash tools/sim/check.sh` 确认原版零回归。
2. **阶段 B（主视觉四页）**：实现 `page_home.cpp` / `page_tokens.cpp` / `page_quota.cpp` / `page_detail.cpp` 的潮汐分支。
3. **阶段 C（复杂交互三页）**：实现 `page_rank.cpp` / `page_settings.cpp` / `page_chat.cpp` 的潮汐分支。
4. **阶段 D（模拟器批量出图 + 全量回归）**：在 `tools/sim/main.cpp` 末尾追加 15 张潮汐 PNG 渲染（输出到 `tools/sim/out/tide_*.png`，不写入 `baseline.txt`）：
   - `tide_home_work.png`、`tide_home_idle.png`、`tide_home_alert.png`
   - `tide_tokens.png`、`tide_quota.png`、`tide_approval.png`
   - `tide_chat_list.png`、`tide_chat_expand.png`、`tide_chat_rec.png`、`tide_chat_draft.png`
   - `tide_settings.png`、`tide_rank.png`、`tide_detail_online.png`、`tide_detail_offline.png`、`tide_aod.png`

### 交卷硬指标（缺一不可）
- [ ] `bash tools/check_all.sh` **全绿通过**（原有 51 项基线零破坏、无裸 `368`、`test_settings` / `test_aod` / `test_proto` / `junit` 全部 PASS）；
- [ ] `./tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8` **固件编译 0 error**；
- [ ] `tools/sim/out/tide_*.png` 全部 15 张图成功生成，且 `tide_aod.png` 为纯黑底；
- [ ] 汇报改动文件列表、构建与测试原始输出，交回 **Carbon** 看图终审（如实机观察到 `.nva` 边缘在钴蓝底上暗边明显，再由 Phase 2 单独补半透边缘色槽）。
