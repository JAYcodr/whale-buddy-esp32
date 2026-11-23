# Nova Buddy 动效上下文（事实摘录）

基线 `84a2746`（2026-11-23 23:37）。行号均按该提交；`src/…:N` 指设备端 C++。范围是设备 UI 与渲染，不含 host-plugin。
本文只记录代码、常量与实测值，不含评价或改进建议。

## 1. 渲染管线

| 任务 | 栈 / 优先级 / 核 | 创建点 | 职责 |
|---|---|---|---|
| `task_ui` | 10240 / 5 / 1 | `src/main.cpp:1271` | 输入分发、动作、逐页绘制、推送、帧率与 DFS |
| `task_decode` | 8192 / 3 / 1 | `src/anim/nva.cpp:403` | zlib + XOR 解帧，写后缓冲 |
| `task_bus` | 4096 / 6 / 0 | `src/hw/bus.cpp:151` | 20 ms 轮询触摸/按键/IMU/电源，投 32 深输入队列 |
| `task_net` | 12288 / 4 / 0 | `src/net/net.cpp:356` | MQTT 收发；不接触画布 |

`task_ui` 单帧入口 `uiTaskFn`（`src/main.cpp:187`），由 `setup()` 创建（`:1271`）；Arduino `loop()` 只 `vTaskDelay(portMAX_DELAY)`（`:1274-1276`）。循环体从 `for (;;)`（`:290`）开始：

```c
for (;;) {
  esp_task_wdt_reset(); nowMs = millis(); pollSerialCommands(...);
  cfg = settingsGet(); snap = hwBusSnapshot();                // 快照，不碰 I2C
  st = novaStateRead();                                        // 整份拷贝
  AOD 状态机: shouldEnter/shouldExit → aodActive, needFull;
  CPU DFS: aodActive?80 : LONG_BATT?(审批160:80) : ADAPTIVE?(活跃240:80);
  while (hwBusPollInput(&ev)) { 触摸/按键 → UiAction → handleAction(); }
  动画状态机（审批>demo>click>oneShot>run>walk>sleep>choreo>bg）;
  nvaTick(nowMs);                                              // 10 fps，通知 task_decode
  needFull |= stateGen/quotaGen/rankGen/choreoGen/tokenGen 变、分钟变、电量 5% 桶、
              burnStep 变、审批秒变/最后 3 s 每 125 ms、整屏浮层、转场;
  targetFps = 页面策略(); frameUs = 1000000 / targetFps;
  if (!screenOff && !dimmedForNap) {
    if (needFull)                 { aodActive ? pageHomeDrawAod : nav.draw; hwDisplayPush(); }
    else if (needSettingsList)    { pageSettingsDrawList; hwDisplayPushRect(0,61,368,387); }
    else if (needRankList)        { pageRankDrawList;     hwDisplayPushRect(0,96,368,295); }
    else if (animPage)            { pageHomeDrawStage;    hwDisplayPushRect(并集矩形); }
    else if (tokensAnim)          { pageTokensDrawStage;  hwDisplayPushRect(0,194,368,88); }
    else if (approvalActive)      { pageApprovalDrawBar;  hwDisplayPushRect(36,92,296,8); }
  }
  fps 统计（1 s 窗口）；nextFrameAt += frameUs；vTaskDelay(到点或 1 ms);
}
```

**脏矩形决策**（`src/main.cpp:964-1006`、`1140-1199`）：`needFull` 为真时走全屏，并清掉三个局部标志；否则按 `needSettingsList → needRankList → animPage → tokensAnim → approvalActive` 的固定顺序只走一个分支。`animPage` 条件含 `!aodActive && !approvalActive && !wifiApMode && 当前页==HOME && !(dockExpanded && turnError && errMsg[0])`（`src/main.cpp:1112-1114`）。

| 分支 | 绘制 | 推送矩形 | 定义点 |
|---|---|---|---|
| 全屏 | `nav.draw()` 或 `pageHomeDrawAod()` | `(0,0,368,448)` | `src/main.cpp:1141-1155` |
| 设置列表 | `pageSettingsDrawList()` | `pageSettingsListRect()` = `(0,61,368,387)` | `src/ui/page_settings.cpp:752` |
| 榜单列表 | `pageRankDrawList()` | `pageRankListRect()` = `(0,96,368,295)` | `src/ui/page_rank.cpp:552` |
| 主屏舞台 | `pageHomeDrawStage()` | 函数回填 `dx/dy/dw/dh` | `src/ui/page_home.cpp:511`、`342-435` |
| Token 鱼群 | `pageTokensDrawStage()` | `(0,194,368,88)` | `src/ui/page_tokens.cpp:538` |
| 审批进度条 | `pageApprovalDrawBar()` | `(36,92,296,8)` | `src/ui/page_approval.cpp:113` |
| 按下反馈 | `drawPressedOverlay()` | 命中矩形 | `src/main.cpp:735-742` |

**双缓冲与刷新**：画布有 1 块 framebuffer（`Arduino_Canvas`，`aligned_alloc(16, 368*448*2)`，`lib/GFX_Library_for_Arduino/src/canvas/Arduino_Canvas.cpp:37-40`）。`hwDisplayPush()`（`src/hw/display.cpp:131`）在 1:1 面板上直接调 `hwDisplayPushRect(0,0,368,448)`（调用在 `:225`，定义在 `:256`）：宽 `rw` 裁剪后做偶对齐、最小宽 18；`rw≥240` 时整行带零拷贝，否则先拷进 64 KB 内部 SRAM 条带缓冲再按条带推（`:287-315`）。最终落到 `Arduino_SH8601::draw16bitRGBBitmap` 覆写（`lib/GFX_Library_for_Arduino/src/display/Arduino_SH8601.cpp:22-29`）：一次 `writeAddrWindow` + `_bus->writePixels(w*h)`。QSPI 80 MHz（`src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h:69`），`ESP32QSPI_MAX_PIXELS_AT_ONCE=8192`（`platformio.ini:45`）；`writePixels` 内部是 `POLL_START/POLL_END` 阻塞轮询，**此路径未用 DMA**。转场另有两块 PSRAM 全帧缓冲 `from/to`（`src/ui/transition.cpp:121-124`），`transRun` 每轮把合成结果写回 fb 后立刻 `hwDisplayPush()`（`:242`），结束时再整帧 `memcpy` + push（`:245-246`）。NVA 帧是独立双缓冲：`s_frameBuf[2]`，解码成功才切 `s_frontIdx`（`src/anim/nva.cpp:266-317`）。

## 2. 现有动画与过渡

| 名称 | 触发 | 时长 / 曲线 | 实现位置 | 每帧重绘区域 |
|---|---|---|---|---|
| 横向滑屏 | 左右滑 ≥48 px（`src/ui/nav.cpp:49`）或 Key1 短按切页 | 180 ms，`1-(1-u)^3` | `src/main.cpp:527`、`src/ui/transition.cpp:136-203` | 全屏；y<46 行固定取目标帧（`transition.cpp:16,158-160`） |
| 设置浮层升起 | 进入 SETTINGS（不在页面环内，入口为 Key1 长按 / 详情页按钮） | 220 ms，同曲线 | `src/main.cpp:523` | 全屏；下半屏滑入，上半屏从 1.0 压到 154/256（0.6）（`:204-221`） |
| 设置浮层落下 | 设置返回箭头（`GO_PAGE DETAIL`） | 220 ms，同曲线 | `src/main.cpp:525` | 全屏（`:222-231`） |
| Key1 离开设置 | Key1 短按（`goHome`） | 180 ms，同曲线 | `src/main.cpp:849-865` | 全屏（走 `SLIDE_RIGHT`，非 SHEET_DOWN） |
| 审批弹窗进/出 | `st.apGen` 变化；批准/拒绝/转电脑端 | 150 ms，全帧 alpha 0↔32 | `src/main.cpp:486,495,587,600,613`、`transition.cpp:233-247` | 全屏交叉淡入淡出 |
| 榜单维度切换 | `RANK_REDRAW` | 150 ms fade | `src/main.cpp:615-618` | 全屏 |
| 按钮按下高亮 | TOUCH_DOWN 命中 `pressedRect` | 无时长，立即 | `src/main.cpp:735-742`、`src/ui/theme.cpp:442-479` | 命中矩形（+18% 亮度，圆角 12） |
| Nova 光环粒子 | HOME 持续 | 相位 `frame*0.06*(busy?1.25:0.65)`，3 点椭圆 | `src/ui/page_home.cpp:391-402` | 舞台并集 |
| Nova 行走跳跃 | `targetX` 与 `petX` 距离 >1.2 px | 帧相位 `sin(frame*0.28)*5 px`；速度 38 / 92 px/s | `page_home.cpp:219-327,337-340` | 舞台并集 |
| Nova 触摸粒子 | 点宠物（距中心 ≤68 px、y≥104） | 24 帧，6 点，半径 44→82 px | `page_home.cpp:404-416,811-813` | 舞台并集 |
| Nova 挤压/回弹 | 点宠物、走到目标 | 320 ms：0–45% ease-in 压到 0.86，45–100% easeOutBack 回到 1.0（过冲上限 1.10） | `page_home.cpp:184-208,419` | 舞台并集 |
| Nova 醒来 | 点 SLEEP 中的宠物（AOD / 熄屏唤醒见 §6） | CLICK 一次性重启 + 醒来台词 5.5 s | `page_home.cpp:814-818` | 舞台并集 |
| Nova 漫游 | 到点且 `nowMs≥nextRoamMs` | 停点 `{108,184,260,140,228,184}`，间隔 13 / 18 s | `page_home.cpp:315-327` | 舞台并集 |
| Nova 帧动画 | 状态机 | 9 套 `.nva`，10 fps，100 帧/循环（error 49 帧） | `src/anim/nva.cpp:450-494` | 舞台并集 |
| 背景动画轮播 | 无审批/无 run/非 demo | 播 1 个循环（约 10 s）切一次；IDLE 停 2 个循环 | `src/main.cpp:1064-1080` | 舞台并集 |
| LLM 编排 | MQTT `choreo` 更新 | 到场后 ≥10 s；结束需 1 个动画循环完成，或再超 5 s 保险 | `page_home.cpp:260-310`、`src/net/proto.cpp:244-268` | 舞台并集 |
| Token 鲸鱼入场 | 进入 TOKENS 页 | 每条第 i*55 ms 起步，750 ms easeOutCubic，x 偏 -140→0，缩放 0.55→1，alpha 0→1（前 60%） | `src/ui/page_tokens.cpp:118-152,386-390` | 鱼群带 |
| Token 鲸鱼漂浮 | 入场后持续 | ±2.6 px，周期 3.6–5.0 s；`bobScale` 可降为 0 | `page_tokens.cpp:146-153,324-340` | 鱼群带 |
| Token 选中 | 点中鲸鱼 | 无过渡，只换颜色（`COL_ACCENT`） | `page_tokens.cpp:504-514`、`src/main.cpp:635-636` | 全屏（`handleAction` 置 `needFull`） |
| 榜单拖动 / 越界 | 竖向拖动 ≥12 px 且 ady>adx | 1:1 跟随；越界乘 0.4，上限 28 px | `page_rank.cpp:306-381` | 榜单列表 |
| 榜单惯性 / 回弹 | 抬起时最近 3 样本速度 ≥0.3 | 0.94/帧衰减，速度上限 80 px/帧；回弹 160 ms easeOutCubic | `page_rank.cpp:383-423,425-494` | 榜单列表 |
| 设置展开/收起 | 点行 | 150 ms easeOutCubic；行高按 `rowH(i)` 插值；选项在 `frac≥0.90` 才画 | `page_settings.cpp:51,191,302-310,756-790,572-578` | 设置列表 |
| 设置滚动 / 惯性 | 竖向拖动 ≥8 px | 与榜单同参数：0.4 阻尼、160 ms 回弹、0.94 衰减 | `page_settings.cpp:46-50,1085-1188,793-855` | 设置列表 |
| 滑块 | 按下展开区即抓取 | 无动画，5% 吸附；拖动中只改内存，松手才落 NVS | `page_settings.cpp:657-676,1049-1069` | 拖动中设置列表；松手后全屏 |
| 开关 | 点行 | 无补间，直接跳变（旋钮到轨道端点） | `src/ui/theme.h:101`、`theme.cpp:345-358` | 全屏（`handleAction` 置 `needFull`） |
| 审批倒计时条 | 审批中 | 宽度线性 `rem/ttl`；非全屏帧按审批目标帧率（30，交互中 60）局部刷新，秒变时全屏 | `page_approval.cpp:41-53`、`src/main.cpp:1191-1198` | 进度条矩形 |
| 审批呼吸边框 | 剩余 ≤3 s | 500 ms 三角波，红↔暗红 | `page_approval.cpp:24-39` | 全屏，每 125 ms 一次（`src/main.cpp:990-1002`） |
| AOD 进出 | `aod::shouldEnter/shouldExit` | 无转场，直接整屏切换 | `src/main.cpp:340-380`、`src/model/aod_policy.h:12-50` | 全屏 |
| AOD 姿势 | run>0 / wait>0 / 其他 | 换姿势重开 0.9 s 解码窗，解出一帧后冻结 | `src/main.cpp:384-393,1084-1088` | 全屏 |
| 防烧屏位移 | 每 30 s | `burnOffset` ±2 px，5×5 序列 | `theme.h:309-318` | 全屏（`burnStep` 变化置 `needFull`） |
| AOD 慢漂移 | 每 150 s | ±3 px，4 落点 | `aod_policy.h:70-77` | 随 AOD 整屏 |

## 3. 缓动与补间工具

| 签名 | 位置 | 调用点 |
|---|---|---|
| `bool transBuffers(uint16_t** from, uint16_t** to)` | `src/ui/transition.cpp:116` | `src/main.cpp:445`（转场前取两块 PSRAM 缓冲） |
| `void transRun(Arduino_Canvas& canvas, const uint16_t* from, const uint16_t* to, TransKind kind, uint32_t ms)` | `transition.cpp:136` | `src/main.cpp:458`（唯一转场执行点） |
| `inline uint16_t dimPixel(uint16_t c, uint32_t scale)` | `transition.cpp:24` | `dimRows` 内部（`:31`） |
| `void dimRows(uint16_t* dst, const uint16_t* src, int rows, int32_t scale)` | `transition.cpp:31` | `transRun` 的 SHEET_UP/DOWN（`:212,226`） |
| `inline uint16_t blendPixel(uint16_t cf, uint16_t ct, uint32_t invA, uint32_t alpha5)` | `transition.cpp:64` | `blendFrame` 内部 |
| `void blendFrame(uint16_t* dst, const uint16_t* from, const uint16_t* to, size_t pxCount, uint32_t alpha5)` | `transition.cpp:71` | `transRun` 的 FADE（`:234`） |
| `float easeOutCubic(float u)`（文件内 static） | `src/ui/page_settings.cpp:191` | `pageSettingsTick` 展开（`:779`）、回弹（`:803`） |
| `float calcSquashScaleY(uint32_t nowMs)` | `src/ui/page_home.cpp:184` | `drawStageContent`（`:419`） |
| `void pageHomeTriggerSquash(uint32_t nowMs)` | `page_home.cpp:506` | 到点（`:253`）；点击置 pending（`:813`）后由 `updateRoam`（`:225`）触发 |
| `inline uint16_t blend565(uint16_t a, uint16_t b, uint8_t t)` | `src/ui/theme.h:91` | 审批呼吸（`page_approval.cpp:35`）、鱼腹（`page_tokens.cpp:194`）、按下（`theme.cpp:476`）、绘制原语 |
| `inline int16_t burnOffsetX(uint8_t step)` / `burnOffsetY(uint8_t step)` | `theme.h:309,314` | `drawTopBar`（`:321-322`）、主屏（`page_home.cpp:333-334`）、AOD（`:728-729`） |
| `inline int16_t aod::driftX(uint32_t nowMs)` / `driftY(uint32_t)` | `src/model/aod_policy.h:70,75` | `pageHomeDrawAod`（`page_home.cpp:728-729`） |

`easeOutCubic` 的公式在四处各写一遍，并非共用函数：`transition.cpp:171`、`page_rank.cpp:440`、`page_settings.cpp:191`、`page_tokens.cpp:141`。token 入场、榜单回弹、设置回弹都用 `1-(1-u)^3`。压缩/回弹曲线在 `page_home.cpp:184-208`，参数 `SQ_DUR_MS=320`、`SQ_MIN=0.86`、`c1=1.70158`。

## 4. 时间基准与帧率

- 主循环时间：`millis()`（`src/main.cpp:292`），写入 `NovaState::nowMs`（`:322`）；页面绘制用 `st.nowMs` 或主循环传入的同值 `nowMs`。
- 输入事件时间：`task_bus` 每轮 `uint32_t now = millis()`（`bus.cpp:55`），随事件写 `InputEvent::tsMs`（`bus.cpp:71-87`）。
- 微秒级计时：`esp_timer_get_time()` 用于单帧 `pushUs`（`src/main.cpp:1138,1149`）、1 s fps 窗口（`:1207-1209`）、帧 deadline（`:1229`）、转场经过时间（`transition.cpp:163`）。
- 帧率控制：`targetFps` 由页面/功耗档决定（`src/main.cpp:1128-1136`），`frameUs=1000000/targetFps`；`nextFrameAt += frameUs`，落后超过一帧则重置为当前，最后 `vTaskDelay(waitUs/1000)`（`:1229-1236`）。
- 实际帧率：`curFps = fpsFrames*1e6/elapsedUs`（`:1210`）；`drawFps` 只在 1 s 窗口内 `fpsFrames≥8` 时更新，否则保留旧值（`:1211-1214`）。

| 条件 | targetFps | 位置 |
|---|---|---|
| 主屏动画（`animPage`） | `baseFps`：自适应 USB 60 / 电池 30；长续航 USB 30 / 电池 15；灵动 60 | `src/main.cpp:1128-1131`、`settings.cpp:121-128` |
| TOKENS 动画 | `min(baseFps,30)` | `src/main.cpp:1129` |
| 触摸中 / fling / 设置动画 / 转场中 | 60 | `src/main.cpp:1125,1133` |
| 审批中（非上述） | 30 | `src/main.cpp:1133` |
| 其他静态页 | 10 | `src/main.cpp:1133` |
| 熄屏 / 翻扣暗屏 | 5 | `src/main.cpp:1134` |
| AOD | 4 | `src/main.cpp:1135` |

dt 计算：主屏漫游 `dtMs = st.nowMs - s_prevRoamMs`，首帧取 33，钳到 ≤120（`page_home.cpp:232-234`）；榜单/设置 fling 用 `rawDt` 钳到 1–50 ms，再换算 `frames = rawDt/16.67`（`page_rank.cpp:447-451`、`page_settings.cpp:807-815`）；转场用 `elapsedUs/durUs`。NVA 解码按包内 10 fps 自排：`stepMs=1000/fps`，`s_nextFrameMs += stepMs`，落后超过 2 步则重置（`nva.cpp:450-461`）。

## 5. 资源与耗时

| 缓冲 / 数据 | 大小 | 位置 / 区域 | 用途 |
|---|---|---|---|
| Canvas framebuffer | 368×448×2 = 329,728 B | `Arduino_Canvas.cpp:37-40`（`aligned_alloc`，代码未显式指定 PSRAM） | 全部绘制 |
| 转场 from / to | 2×329,728 B | `transition.cpp:121-124` | 水平滑屏 / 浮层 / fade 源帧与目标帧 |
| 显示条带缓冲 | 65,536 B | `src/hw/display.cpp:296`（`MALLOC_CAP_INTERNAL`） | `rw<240` 的局部推屏 |
| NVA 帧缓冲 | 2×49,280 B（`220*224`） | `nva.cpp:87,384-385`（`malloc`，未指定 PSRAM） | 索引帧双缓冲 |
| NVA XOR scratch | 49,280 B | `nva.cpp:386`（`allocBuffer(..., true)`，优先 PSRAM） | 帧差解码 |
| 常驻 NVA 包 | idle 543,052 + success 520,296 + error 217,649 B | `nva.cpp:45-47,370-396`（优先 PSRAM） | 状态切换不读 flash |
| 按需 NVA 包 | 最大 quota 701,289 B | `nva.cpp:49-52`（优先 PSRAM） | 单槽流式包 |
| 字体 | 源文件 6,991,372 B | `src/ui/aafont_data.cpp`；编译进 flash | 4 bpp CJK / DIN |

显式 PSRAM 合计：转场 659,456 + XOR scratch 49,280 + 三常驻包 1,280,997 = 1,989,733 B；按需包另计 ≤701,289 B。framebuffer 与 NVA 帧缓冲的落点由分配器决定，代码未显式指定。

实测（`HANDOFF.md:142-146`，真机 USB 串口 `stat`）：插电首页 60 fps / 240 MHz / `pushUs` 10.5 ms / UI 忙 52–64% / 解码忙 4–14%；AOD 80 MHz / `pushUs` 51.8 ms / 5–16 次绘制每分钟（无 state 事件 5 次，有会话活动 16 次）。2026-11-23 23:42 现场采样（`power_probe --watch`）：`aod=1`、80 MHz、亮度 39%、`pushUs` 53.6–53.9 ms、`fps` 0–1、`drawFps` 30（该字段保留上次有效窗口值）。提交 `fac9867` 记录：TOKENS 单帧 40→29.4 ms、30 fps；HOME 24→10.5 ms、60 fps；RANK 全页 115→12 ms；flash 32.5%（2.73 MB/8 MB）、RAM 21.7%。

CPU 降频（`src/main.cpp:412-423`）：AOD 恒 80 MHz；长续航：熄屏/暗屏 80、审批 160、其余 80；自适应：熄屏/暗屏或（电池且无活跃需求）80，否则 240；灵动恒 240。`activeDemand = approvalActive || run>0 || wait>0 || 交互后 10 s 内`。转场开始前显式 `applyCpuMhz(240)`（`:454`）。

## 6. 触摸事件模型

链路：`task_bus` 每 20 ms（5×4 ms 片，触摸 IRQ 可提前打断，`bus.cpp:139-144`）调 `hwInputUpdate()`，变化经 `pushEvt()`（`bus.cpp:19-25`）写入 32 深队列；`task_ui` 每轮 `while (hwBusPollInput(&ev))` 排空（`src/main.cpp:705-706`），按 `InputEvent.type` 分发。坐标已由 `input.cpp:164-188` 从物理坐标换算到 368×448 画布坐标（1.8 上 `OFFSET=0`、`SCALE=1`）；`tsMs` 为投递时的 `millis()`。点击在 **TOUCH_UP** 判定，除滑块/榜单/设置的 DOWN 预处理外，DOWN 只负责按下反馈与记录起点。

| 判定 | 阈值 / 条件 | 位置 |
|---|---|---|
| 按下反馈出现 | DOWN 命中 `nav.pressedRect()` | `src/main.cpp:735-742` |
| 按下反馈取消 | 位移 ≥12 px（任轴） | `src/main.cpp:747-750` |
| 竖向滚动（榜单） | `ady≥12 && ady>adx` | `src/main.cpp:752-759` |
| 竖向滚动（设置） | `ady≥8 && ady>adx` | `src/main.cpp:765-773` |
| 设置页抬起后不派发 tap | `adx≥16` 或 `ady≥16` | `src/main.cpp:831` |
| 左右切页 | `adx≥48 && adx>ady`，SETTINGS 页直接返回 NONE | `nav.cpp:41-62` |
| 滑块抓取 | DOWN 落在已展开行的滑块区 | `src/main.cpp:731-733`、`page_settings.cpp:1049-1053` |
| Key1 短按 | 松开且未触发长按 | `src/main.cpp:849-865` |
| Key1 长按 | 按住 ≥800 ms | `src/main.cpp:26,882-890` |
| AXP 短按 | AOD 退出 / 熄屏唤醒 / 亮屏熄屏 | `src/main.cpp:867-875` |
| AOD 唤醒 | 触摸先退出 AOD，并吞掉这一击（`swallowTap`） | `src/main.cpp:713-720,779-782` |

手势结果统一为 `UiAction`：`nav.onSwipe()` 返回 `GO_PAGE`，`nav.onTap()` 调当前页 `pageXxxTap()`；`handleAction`（`src/main.cpp:505`）执行切页、转场、设置落盘、审批决策等。主屏自身还处理：点中宠物 → `PET_TAP value=0`（CLICK 一次性 + 粒子 + 挤压 + 台词）；点舞台其他位置 → `PET_TAP value=1`（走到该 x）；点状态胶囊 → `PET_TAP value=2`（循环 demo 状态）；异常时点码头 → `DOCK_TOGGLE`。

## 7. 页面框架

没有页面基类。页面是 `src/ui/pages.h` 里的自由函数集合，`UiNav` 持有当前页并做分发。`src/ui/pages.h:8-15`：

```cpp
enum class UiPage : uint8_t {
  HOME     = 0,
  QUOTA    = 1,
  RANK     = 2,
  DETAIL   = 3,
  SETTINGS = 4,
  TOKENS   = 5,
};
```

`src/ui/nav.h:8-23`：

```cpp
class UiNav {
public:
  void init(HomePageKind home);
  UiPage currentPage() const { return m_page; }
  void setPage(UiPage p);
  void goHome(HomePageKind home);

  UiAction onSwipe(int16_t dx, int16_t dy);
  UiAction onTap(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg);
  bool pressedRect(int16_t x, int16_t y, const NovaState& st, const NovaSettings& cfg, ui::Rect* out) const;

  void draw(NovaCanvas& c, const NovaState& st, const NovaSettings& cfg, uint32_t frame, uint32_t nowMs);

private:
  UiPage m_page = UiPage::HOME;
};
```

页面接口约定（`pages.h:56-129`）：`void pageXxxDraw(NovaCanvas&, const NovaState&, …)`、`UiAction pageXxxTap(int16_t, int16_t, …)`、`bool pageXxxHitRect(int16_t, int16_t, const NovaState&, const NovaSettings&, ui::Rect*)`；可局部重绘的页另有 `pageHomeDrawStage(..., int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH)`、`pageRankDrawList()`、`pageSettingsDrawList()`、`pageTokensDrawStage()`。`UiNav::draw()` 的分发在 `nav.cpp:94-107`；审批浮层优先于当前页（`nav.cpp:96-99`）。

切页 API：`nav.setPage(p)` / `nav.goHome(kind)` 只改 `m_page`，不画；绘制由下一轮 `needFull` 触发。带转场的切页走 `UiAction::GO_PAGE` → `handleAction`（`src/main.cpp:509-529`）：先 `setPage`，再按情形调 `playTransition(SHEET_UP/SHEET_DOWN/SLIDE_LEFT/SLIDE_RIGHT, 220/220/180)`。页面环是 `TOKENS, HOME, QUOTA, RANK, DETAIL` 五页（`nav.cpp:4-10`）；SETTINGS 不在环里，是模态层；审批是覆盖层；AOD 在主循环里整屏接管。

左滑 = 环上下一个（`nav.cpp:52-54`），右滑 = 上一个（`:55-58`）；SETTINGS 页 `onSwipe` 直接返回 NONE（`nav.cpp:42-46`），垂直手势归列表。顶部 5 个页面圆点由 `drawPagePills`（`theme.h:292`）在 `drawTopBar` 中绘制（`theme.h:364`），无点击命中。

## 8. 构建、烧录、截图与 HANDOFF 要点

```bash
./tools/prep_data.sh                                   # assets → data（LittleFS 内容）
./tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8 -t uploadfs --upload-port /dev/cu.usbmodemXXXX
./tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8 -t upload   --upload-port /dev/cu.usbmodemXXXX
./tools/sim/run.sh                                     # 主机模拟器，PNG 落 tools/sim/out/
./tools/sim/check.sh [--update]                        # framebuffer 哈希基线
bash tools/check_all.sh                                # 电池/AOD/协议/设置/插件/模拟器
.venv-shot/bin/python tools/usb_shot.py --port /dev/cu.usbmodem21101 --out /tmp/x.png
```

串口 JSON（`HANDOFF.md:225-242`）：`stat`、`bat`、`shot`、`page`、`tap`、`demo`、`aod`、`aodidle`、`aodforce`、`statrate`、`pwr`。`page` 命令直接 `nav.setPage` + `needFull`（`src/main.cpp:125-133`），不经过 `playTransition`；`tap` 走 `nav.onTap` + `handleAction`，与真实触屏同路径。`shot` 输出 `NVSH` 头 + RGB565 全帧；`stat` 含 `fps / drawFps / pushUs / idle / draws / mhz / aod / cpu{ui,dec}`。

HANDOFF 与动效直接相关的条目：浮层与驱动动画必须禁局部重绘（`HANDOFF.md:76-79`）；设置页 150 ms easeOutCubic、行高组 38 / 行 62 / 3 项 +62 / 2×2 +114 / 滑块 +68（`:89`）；AOD 进入只看端侧输入、姿势与 0.9 s 解码窗、防烧屏位移（`:132-148`）；未做的省电项清单（`:172`）；AOD 侧七条“别踩回去”（`:181-189`）；`animPage` 生效时 MQTT state 会被宠物局部重绘覆盖（`:244`）。构建开关含 `-O2`、`-fno-math-errno`、`-DESP32QSPI_MAX_PIXELS_AT_ONCE=8192`（`platformio.ini:45-49`）。

## 9. 代码原文

### 9.1 滑屏合成（`src/ui/transition.cpp:136-247`；空行已删）

```cpp
void transRun(Arduino_Canvas& canvas, const uint16_t* from, const uint16_t* to,
              TransKind kind, uint32_t ms) {
  uint16_t* fb = canvas.getFramebuffer();
  const size_t pxCount = (size_t)UI_W * UI_H;
  const size_t fbBytes = pxCount * sizeof(uint16_t);
#ifdef NOVA_HOST_SIM
  (void)from;
  (void)kind;
  (void)ms;
  if (fb && to) memcpy(fb, to, fbBytes);
  return;
#else
  if (!fb || !from || !to) {
    if (fb && to) {
      memcpy(fb, to, fbBytes);
      hwDisplayPush();
    }
    return;
  }
  if (kind == TransKind::SLIDE_LEFT || kind == TransKind::SLIDE_RIGHT) {
    // Keep top status/clock band fixed from destination frame during horizontal slides.
    memcpy(fb, to, (size_t)kSlideSplitY * UI_W * sizeof(uint16_t));
  }
  const int64_t tStartUs = esp_timer_get_time();
  const int64_t durUs    = (int64_t)ms * 1000LL;
  while (durUs > 0) {
    int64_t elapsedUs = esp_timer_get_time() - tStartUs;
    if (elapsedUs >= durUs) break;
    if (elapsedUs < 0) elapsedUs = 0;
    float u = (float)elapsedUs / (float)durUs;
    float inv = 1.0f - u;
    float ease = 1.0f - inv * inv * inv;
    switch (kind) {
      case TransKind::SLIDE_LEFT:
      case TransKind::SLIDE_RIGHT: {
        int offset = (int)(ease * UI_W + 0.5f);
        if (offset < 0) offset = 0;
        if (offset > UI_W) offset = UI_W;
        int keepFrom = UI_W - offset;
        bool enterFromRight = (kind == TransKind::SLIDE_LEFT);
        for (int y = kSlideSplitY; y < UI_H; y++) {
          uint16_t* dstRow = fb + (size_t)y * UI_W;
          const uint16_t* srcFrom = from + (size_t)y * UI_W;
          const uint16_t* srcTo   = to   + (size_t)y * UI_W;
          if (enterFromRight) {
            if (keepFrom > 0) {
              memcpy(dstRow, srcFrom + offset, (size_t)keepFrom * sizeof(uint16_t));
            }
            if (offset > 0) {
              memcpy(dstRow + keepFrom, srcTo, (size_t)offset * sizeof(uint16_t));
            }
          } else {
            if (offset > 0) {
              memcpy(dstRow, srcTo + keepFrom, (size_t)offset * sizeof(uint16_t));
            }
            if (keepFrom > 0) {
              memcpy(dstRow + offset, srcFrom, (size_t)keepFrom * sizeof(uint16_t));
            }
          }
        }
        break;
      }
      case TransKind::SHEET_UP: {
        int offset = (int)(ease * UI_H + 0.5f);
        if (offset < 0) offset = 0;
        if (offset > UI_H) offset = UI_H;
        int splitY = UI_H - offset;
        int32_t scale = 256 - (int32_t)(ease * (float)kDimSpan + 0.5f);
        if (scale < kDimMinScale) scale = kDimMinScale;
        if (scale > 256) scale = 256;
        if (splitY > 0) dimRows(fb, from, splitY, scale);
        if (offset > 0) {
          memcpy(fb + (size_t)splitY * UI_W, to, (size_t)offset * UI_W * sizeof(uint16_t));
        }
        break;
      }
      case TransKind::SHEET_DOWN: {
        int offset = (int)(ease * UI_H + 0.5f);
        if (offset < 0) offset = 0;
        if (offset > UI_H) offset = UI_H;
        int remainFrom = UI_H - offset;
        int32_t scale = kDimMinScale + (int32_t)(ease * (float)kDimSpan + 0.5f);
        if (scale < kDimMinScale) scale = kDimMinScale;
        if (scale > 256) scale = 256;
        if (offset > 0) dimRows(fb, to, offset, scale);
        if (remainFrom > 0) {
          memcpy(fb + (size_t)offset * UI_W, from,
                 (size_t)remainFrom * UI_W * sizeof(uint16_t));
        }
        break;
      }
      case TransKind::FADE: {
        int alpha5 = (int)(ease * 32.0f + 0.5f);
        if (alpha5 < 0) alpha5 = 0;
        if (alpha5 > 32) alpha5 = 32;
        blendFrame(fb, from, to, pxCount, (uint32_t)alpha5);
        break;
      }
    }
    hwDisplayPush();
  }
  memcpy(fb, to, fbBytes);
  hwDisplayPush();
#endif
```

### 9.2 审批弹窗绘制（`src/ui/page_approval.cpp:55-96`；空行已删）

```cpp
void pageApprovalDraw(NovaCanvas& c, const NovaState& st, uint32_t nowMs) {
  c.fillScreen(ui::COL_BG);
  ApprovalTiming t = computeTiming(st, nowMs);
  ui::drawCard(c, 20, 32, 328, 394, ui::COL_CARD, t.frameCol, 22);
  // Alert indicator & large numeric countdown
  ui::aaFillCircle(c, 42, 62, 5, t.frameCol);
  ui::drawText(c, ui::fontCjk16(), 56, 69, "请求工具确认", ui::COL_TEXT, ui::COL_CARD);
  char secStr[16];
  snprintf(secStr, sizeof(secStr), "%us", (unsigned)((t.rem + 999) / 1000));
  ui::drawTextRight(c, ui::fontNum24(), 330, 74, secStr, t.frameCol, ui::COL_CARD);
  // Smooth countdown progress bar
  pageApprovalDrawBar(c, st, nowMs);
  // Tool header card
  ui::drawCard(c, 36, 112, 296, 46, ui::COL_CARD_HI, ui::COL_BORDER, 12);
  ui::aaFillRoundRect(c, 46, 122, 48, 26, 7, rgb565(18, 44, 68), rgb565(18, 44, 68));
  ui::drawTextCenteredInRect(c, ui::fontCjk14(), { 46, 122, 48, 26 }, "工具", ui::COL_ACCENT, rgb565(18, 44, 68));
  char toolBuf[48];
  ui::fitUtf8Line(c, ui::fontAscii18(), st.apTool[0] ? st.apTool : "Tool", 216, toolBuf, sizeof(toolBuf));
  ui::drawText(c, ui::fontAscii18(), 104, 143, toolBuf, ui::COL_TEXT, ui::COL_CARD_HI);
  // Multi-line reason box (UTF-8 safe wrap)
  ui::drawCard(c, 36, 168, 296, 122, rgb565(8, 12, 20), ui::COL_BORDER, 12);
  const char* reason = st.apReason[0] ? st.apReason : "DSH 请求执行该操作";
  ui::drawWrappedUtf8(c, ui::fontCjk14(), 48, 195, 272, 24, 4, reason, ui::COL_TEXT, rgb565(8, 12, 20));
  // Deny / Allow primary buttons
  const uint16_t denyBg = rgb565(46, 14, 22);
  ui::aaFillRoundRect(c, BTN_DENY.x, BTN_DENY.y, BTN_DENY.w, BTN_DENY.h, 14, denyBg, ui::COL_RED);
  ui::drawTextCenteredInRect(c, ui::fontCjk16(), BTN_DENY, "拒绝", ui::COL_RED, denyBg);
  ui::drawButton(c, BTN_ALLOW, "批准执行", true, ui::COL_MINT, ui::fontCjk16());
  // Secondary action: handle on desktop
  ui::drawButton(c, BTN_TO_PC, "转电脑端处理", false, ui::COL_DIM, ui::fontCjk14());
}
```
