# Nova Buddy 宠物表现力升级任务书（v0.6.0 · 24 动作扩容）

> **定位**：架构契约 + 分模型执行任务单（配合《模型分工速查 2026-11-25 版》使用）
> **基线**：仓库 `/Volumes/hamelin-ssd/gemini工作区/nova-buddy` · 分支 `main`（HEAD `5ed1917`）
> **素材源**：`/Volumes/hamelin-ssd/gemini工作区/dsh-pet/dsh-pet/assets/webm/`（`CC BY-NC-SA 4.0`，已在 `README.md` 署名）
> **必读前置**：动手前先读 [`AGENTS.md`](../AGENTS.md) 与 [`docs/ARCHITECTURE.md`](ARCHITECTURE.md)（尤其是 §6 不变量与坑）。

---

## 0. 核心决策与硬件边界核查

1. **Flash 分区解锁（8MB → 16MB）**：
   - Waveshare `ESP32-S3-Touch-AMOLED-1.8` 板载物理 Flash 为 **16MB**（芯片 `ESP32-S3R8` 捆绑 8MB OPI PSRAM + 外置 16MB QIO Flash）。此前沿用 8MB 分区表导致 LittleFS 仅 `4.94MB`（9 个 `.nva` 占 `4.15MB`，已塞满）。
   - 新增 [`no_ota_16mb.csv`](../no_ota_16mb.csv)，仅在 `[env:waveshare-esp32s3-touch-amoled-1-8]` 启用（其余 1.75c / 2.16 板配置保持原 `no_ota_8mb.csv` 不动）：
     ```csv
     # Name,   Type, SubType, Offset,   Size
     nvs,      data, nvs,     0x9000,   0x6000
     phy_init, data, phy,     0xf000,   0x1000
     factory,  app,  factory, 0x10000,  0x3F0000
     spiffs,   data, spiffs,  0x400000, 0xC00000
     ```
   - `spiffs` 标签必须保留（Arduino `LittleFS.begin()` 默认按 label `"spiffs"` 挂载）；容量扩至 **12MB（`0xC00000`）**，装下 24 个高清 `NVA2` 动画（预计总计 ~10.2MB）仍有近 2MB 余量。
2. **解码帧缓冲安全边界（实测全过，零改 SRAM）**：
   - 经对 `dsh-pet` 候选源视频按 `world_scale = 0.5956`（站立高 162px）实测包围盒，新增 15 个动作全部满足 `w * h <= 39,964`，远低于 [`src/anim/nva.cpp:87`](../src/anim/nva.cpp#L87) 的 `MAX_FRAME_PIXELS = 220 * 224 = 49,280`（最大为 `hum` 悠闲哼歌 `206×194 = 39,964 px`）。**无需扩大内部 SRAM 双缓冲**，PSRAM 常驻槽维持原有的 3 个（`IDLE / SUCCESS / ERROR`），其余走按需流式槽 `s_streamPack`，内存水位零劣化。
3. **字库安全边界**：
   - 新增台词与胶囊文案只能用 GB2312 一级常用字及 [`tools/fonts/gen_aafonts.py`](../tools/fonts/gen_aafonts.py) 已收录标点（`~ ! . … ·`），**严禁写入 Emoji 或生僻字**（否则渲染为空白）。

---

## 1. 统一接口契约：24 动作枚举与映射表（Single Source of Truth）

所有端（Python 资产转换、C++ `NvaAnimKey`、C++ `proto.cpp`、Node `ai.js`、UI `page_home.cpp`）**必须严格按以下 `0..23` 顺序与键名对齐**，前 9 项（`0..8`）顺序与原版 100% 兼容：

| 索引 | 枚举名 `NvaAnimKey` | JSON/文件名 (`key`) | `dsh-pet` 源文件名（`.webm`） | 镜像 `allowMirror` | 胶囊展示名 `kDemoTags` | 触发语义与编排定位 |
|---:|---|---|---|---|---|---|
| 0 | `IDLE` | `idle` | `待机呼吸休闲` | ✅ | `展示: 待机` | 默认呼吸待机 |
| 1 | `THINKING` | `thinking` | `工作状态-思考冒泡` | ❌ | `展示: 思考` | `run>0` 且尚未调工具 / 思考中 |
| 2 | `WORKING` | `working` | `工作状态-忙碌点按` | ❌ | `展示: 忙碌` | `run>0` 通用工具执行中 |
| 3 | `WAITING` | `waiting` | `工作状态-原地踱步张望` | ❌ | `展示: 等待` | `wait>0` / 审批等待 |
| 4 | `SUCCESS` | `success` | `工作状态-雀跃庆祝` | ✅ | `展示: 庆祝` | 任务完成 / 批准放行（one-shot） |
| 5 | `ERROR` | `error` | `工作状态-垂头叹气冒汗` | ❌ | `展示: 委屈` | 任务异常 / 拒绝拦截（one-shot） |
| 6 | `CLICK` | `click` | `点击回应-元气挥手` | ✅ | `展示: 挥手` | 戳宠物回应池 #1 |
| 7 | `SLEEP` | `sleep` | `睡觉-循环` | ✅ | `展示: 熟睡` | 闲置熟睡循环 / AOD 默认定格 |
| 8 | `QUOTA` | `quota` | `余额-钱袋满溢` | ❌ | `展示: 富足` | 余额充裕（DeepSeek ≥ 20 元） |
| 9 | `CLICK_HAPPY` | `click_happy` | `点击回应-开心跃动` | ✅ | `展示: 跃动` | 戳宠物回应池 #2 |
| 10 | `CLICK_SHY` | `click_shy` | `点击回应-害羞惊讶` | ✅ | `展示: 害羞` | 戳宠物回应池 #3 |
| 11 | `CLICK_ANGRY` | `click_angry` | `点击回应-傲娇生气` | ❌ | `展示: 傲娇` | 10 秒内连戳 ≥ 3 次触发傲娇生气 |
| 12 | `CLICK_GIGGLE` | `click_giggle` | `点击回应-挠痒咯咯笑` | ✅ | `展示: 挠痒` | 戳宠物回应池 #4 |
| 13 | `CRAB_WALK` | `crab_walk` | `螃蟹走路` | ✅ | `展示: 蟹步` | `walk` 慢速移动时的专属步伐 |
| 14 | `HAPPY_RUN` | `happy_run` | `开心小跑` | ✅ | `展示: 小跑` | `run` 快速跑向目标点时的专属步伐 |
| 15 | `CODING` | `coding` | `写代码` | ❌ | `展示: 敲码` | `run>0` 且 `lastTool` 为代码/终端类工具 |
| 16 | `ARCHIVE` | `archive` | `工作状态-清点归档` | ❌ | `展示: 归档` | `run>0` 且 `lastTool` 为搜索/读取类工具，或剧本使用 |
| 17 | `STRETCH` | `stretch` | `超大伸懒腰` | ✅ | `展示: 懒腰` | 久坐/闲置过渡、AI 剧本常客 |
| 18 | `YAWN` | `yawn` | `哈欠连天` | ✅ | `展示: 哈欠` | 入睡前 15s 犯困预警 / 深夜剧本 |
| 19 | `WAKE_UP` | `wake_up` | `打瞌睡被惊醒` | ✅ | `展示: 惊醒` | 在 `SLEEP/YAWN` 态被戳醒时的专属反应（one-shot） |
| 20 | `HUM` | `hum` | `悠闲哼歌` | ✅ | `展示: 哼歌` | 日常摸鱼剧本 / 本地自动巡游池 |
| 21 | `EAT_TOKEN` | `eat_token` | `吃Token` | ❌ | `展示: 干饭` | 今日烧掉大量 Token 时 / 摸鱼干饭剧本 |
| 22 | `BUBBLE` | `bubble` | `鲸鱼吐泡泡特效` | ✅ | `展示: 冒泡` | 鲸鱼娘招牌动作 / 本地自动巡游池 |
| 23 | `QUOTA_WORRY` | `quota_worry` | `余额-数金皱眉` | ❌ | `展示: 算账` | 余额告急（DeepSeek > 0 且 < 15 元）或订阅 ≥ 80% |

- `NvaAnimKey::COUNT = 24`。

---

## 2. 模型排兵布阵（按《模型分工速查》派活）

| 阶段 | 执行模型 | 档位设置 | 负责内容 | 为什么派给它 |
|---|---|---|---|---|
| **Step 0（已完成）** | **Gemini 4 Carbon** | Low | 硬件 16MB 核查、24 动作包围盒实测、接口契约与分包任务书 | 架构把关、掀桌找前提漏洞（避开 8MB 假瓶颈与 `quota_empty` 超宽坑） |
| **Step 1（任务单 A）** | **DeepSeek V4.1 Flash** | **锁死 Max** | 分区表 `no_ota_16mb.csv`、`convert_pet.py` 转换 24 个 `.nva`、固件底层 `nva/proto` 枚举扩展、`host-plugin/lib/ai.js` 导演 Prompt 升级 + 全套 C++/Node 单测 | 纯脚本 + C++ 数据层 + Node 逻辑 + 跑测试修报错，200 亿免费子弹火力全开，不碰 UI 像素与手感 |
| **Step 2（任务单 B）** | **Gemini 4 Hydrogen** | **High** | `page_home.cpp` 与 `main.cpp` 交互与表现力闭环：连戳傲娇、戳醒惊醒、蟹步/小跑移动联动、工具名驱动 `CODING/ARCHIVE/WORKING`、犯困 `YAWN→SLEEP` 过渡、模拟器基线核对与真机烧录截图 | 跨文件状态联动 + 舞台交互手感 + 像素防溢出核对，Hydrogen 工程 71.1 + 视觉 36 最对口 |
| **Step 3（终审）** | **Gemini 4 Carbon** | **Medium** | 对照架构不变量（AOD 禁 `nvaSetState`、局部重绘矩形、防烧屏偏移、零裸 `368`）做最终 Code Review 并合并 `main` | 5% 额度收尾把关 |

> **并行 / 串行说明**：**先跑任务单 A，全绿提交后再跑任务单 B**（B 依赖 A 生成的 24 个 `.nva` 资产与 `NvaAnimKey` 枚举）。建议在分支 `feature/v060-pet-expressiveness` 上施工。

---

## 3. 可直接复制派活的任务单

---

### 📋 任务单 A —— 交给 `DeepSeek V4.1 Flash`（档位：**开启 Max**）

> 直接复制下方代码块发给 DeepSeek V4.1 Flash (Max)：

```markdown
请在 `/Volumes/hamelin-ssd/gemini工作区/nova-buddy` 仓库执行「Nova Buddy v0.6.0 表现力升级 — 资产与协议底座（任务单 A）」。

### 必读规则
1. 动手前先读 `AGENTS.md`、`docs/ARCHITECTURE.md`（重点看 §3 协议与 §6 不变量）以及 `docs/pet-expressiveness-plan.md` 的 §0 和 §1 契约表。
2. 切新分支施工：`git checkout -b feature/v060-pet-expressiveness`（若已存在则直接切过去）。
3. **严禁修改任何 UI 页面布局、配色或触控逻辑**（不碰 `src/ui/page_*.cpp` 和 `src/main.cpp`，但若 `page_home.cpp` / `main.cpp` 因枚举上限硬编码 `8` 或 `9` 影响编译/测试，只允许把硬编码数字改为 `(int)NvaAnimKey::COUNT - 1` 或补齐 `kAnimBaseScale` 数组，功能状态机留给下一棒）。
4. Git 提交规范：必须使用 Agent 自身身份（例如 `-c user.name="DeepSeek" -c user.email="deepseek@agent.com"`，严禁使用 hamelin 身份），提交信息格式：
   `[新增] 标题` + 换行 `原因：… / 影响：… / 验证：…`

### 具体施工清单

#### A1. 16MB Flash 分区表（仅限 1.8 寸 S3 板）
1. 新建 `no_ota_16mb.csv`：
   ```csv
   # Name,    Type, SubType,  Offset,   Size
   nvs,       data, nvs,      0x9000,   0x6000
   phy_init,  data, phy,      0xf000,   0x1000
   factory,   app,  factory,  0x10000,  0x3F0000
   spiffs,    data, spiffs,   0x400000, 0xC00000
   ```
   （保留 `spiffs` label 不变，否则 `LittleFS.begin()` 挂载失败！）
2. 在 `platformio.ini` 的 `[env:waveshare-esp32s3-touch-amoled-1-8]` 段显式覆盖：
   - `board_upload.flash_size = 16MB`
   - `board_build.partitions = no_ota_16mb.csv`
   其余 `env` 保持 `8MB` 和 `no_ota_8mb.csv` 不动。

#### A2. 转换全部 24 个高清 NVA2 动画资产
1. 修改 `tools/assets/convert_pet.py` 中的 `CORE` 字典，按 `docs/pet-expressiveness-plan.md` §1 表格严格扩充为 24 个键值（0..23 顺序一致）：
   - 原有 9 个保持不变（`idle`, `thinking`, `working`, `waiting`, `success`, `error`, `click`, `sleep`, `quota`）。
   - 新增 15 个：`click_happy`(`点击回应-开心跃动`), `click_shy`(`点击回应-害羞惊讶`), `click_angry`(`点击回应-傲娇生气`), `click_giggle`(`点击回应-挠痒咯咯笑`), `crab_walk`(`螃蟹走路`), `happy_run`(`开心小跑`), `coding`(`写代码`), `archive`(`工作状态-清点归档`), `stretch`(`超大伸懒腰`), `yawn`(`哈欠连天`), `wake_up`(`打瞌睡被惊醒`), `hum`(`悠闲哼歌`), `eat_token`(`吃Token`), `bubble`(`鲸鱼吐泡泡特效`), `quota_worry`(`余额-数金皱眉`)。
2. 运行转换脚本（源目录 `/Volumes/hamelin-ssd/gemini工作区/dsh-pet/dsh-pet/assets/webm`，输出 `assets/pets/nova`，带 `--preview`）：
   - 校验生成的 `assets/pets/nova/manifest.json`：共 24 个动作，每个动作的 `w * h <= 49280`（即 `220*224`），总大小 `< 11.5 MB`。
3. 运行 `bash tools/prep_data.sh` 同步到 `data/pets/nova/`。

#### A3. 固件动画与协议层扩展
1. `src/anim/nva.h`：`enum class NvaAnimKey : uint8_t` 按契约表顺序扩充至 24 项（`0..23`，`COUNT = 24`）。
2. `src/anim/nva.cpp`：
   - `ANIM_NAMES` 按相同顺序扩充至 24 个字符串；
   - `kAnimBaseScale` 扩充至 24 个 `1.00f`；
   - 常驻 PSRAM 槽 `isResident()` 保持 `IDLE / SUCCESS / ERROR` 3 个不变，不增加额外常驻内存。
3. `src/net/proto.cpp`：
   - `animNameToIdx(const char* s)` 支持全部 24 个动作名映射为 `0..23`（未知回落 `0`）。
4. `tools/test_proto.cpp`：补充单测，断言全部 24 个 `choreo` 动作名经 `protoApplyMessage("choreo", ...)` 解析后 `st.choreoSteps[i].anim` 精确等于对应枚举值，未知名称回落 `0`。

#### A4. 宿主 AI 导演（`host-plugin`）升级与单测
1. `host-plugin/lib/ai.js`：
   - `VALID_ANIMS` 扩充至全部 24 个动作名。
   - 更新 `DIVERSE_DAY_POOL`（加入 `stretch`, `hum`, `bubble`, `eat_token`, `coding`, `archive`, `click_happy` 等）与 `DIVERSE_NIGHT_POOL`（加入 `yawn`, `stretch`, `sleep`, `hum`, `bubble` 等）。
   - 更新 `callOneLlm` 的导演 system prompt：清晰分组说明新增动作语义（如：余额紧张用 `quota_worry`、Token 消耗高用 `eat_token`、深夜犯困先 `yawn` 再 `sleep`、日常摸鱼多用 `hum / bubble / stretch / click_happy`），并保持 JSON 契约不变。
2. `host-plugin/lib/bridge.js`：
   - `buildContextSummary` 追加今日 Token 消耗简报（若 `tokensPayload?.days?.length > 0` 取最后一天数值），让 AI 导演能感知今天烧了多少 Token 来编排 `eat_token`。
3. 在 `host-plugin/test/` 下补充针对新 24 动作 `normalizeChoreo`（含合法保留、非法回退 `idle`、首尾与相邻防重复替换）的单元测试。

### 验收闭环（必须全部跑通贴出结果）
1. `bash tools/check_all.sh` 全部 PASS（含 proto / battery / aod / settings / state payload / rec / host junit / 47 张 sim 像素基线 / 裸 368 检查）。
2. 检查 `data/pets/nova/*.nva` 数量为 24，且 `du -sh data/` 小于 `11.5M`。
3. 完成后按规范创建干净的 Git Commit。
```

---

### 📋 任务单 B —— 交给 `Gemini 4 Hydrogen`（档位：**High**）

> 等任务单 A 完成提交后，直接复制下方代码块发给 Gemini 4 Hydrogen (High)：

```markdown
请在 `/Volumes/hamelin-ssd/gemini工作区/nova-buddy` 的 `feature/v060-pet-expressiveness` 分支继续执行「Nova Buddy v0.6.0 表现力升级 — 舞台交互与状态机联动（任务单 B）」。

### 必读前置
1. 先读 `AGENTS.md`、`docs/ARCHITECTURE.md`（重点看 §2.4 单帧循环、§5 渲染管线、§6 不变量）、`HANDOFF.md`（AOD 与浮层禁局部重绘铁律）以及 `docs/pet-expressiveness-plan.md` §1 的 24 动作契约表。
2. **硬红线**：
   - `src/` 下禁止出现裸 `368`（用 `UI_W`）；
   - 所有新增/修改的绘制与点击热区必须遵守 `burnOffsetX/Y(st.burnInStep)`；
   - AOD 期间（`aodActive`）严禁在主状态机调用 `nvaSetState`；任何局部重绘分支必须带 `!aodActive` 且避开 `dockExpanded && turnError` 浮层；
   - 新增中文台词严禁含 Emoji 或 GB2312 一级字库以外的生僻字，且任何状态消息不许出现「异常」二字（防误触发 `proto.cpp` 兜底判错）。

### 具体施工清单

#### B1. 多样化戳戳乐 + 连戳傲娇 + 戳醒惊醒（`src/ui/page_home.cpp` & `src/main.cpp`）
1. **日常单戳轮换 / 随机回应**：
   - 在 `IDLE / THINKING / WORKING / CODING / ARCHIVE / HUM / BUBBLE / STRETCH` 等清醒态下点按诺瓦：从 `{CLICK, CLICK_HAPPY, CLICK_SHY, CLICK_GIGGLE}` 4 个回应动作中轮换（避免连播同一个），配对应台词池（如 `"在呢主人!"`, `"嘿嘿，好痒呀~"`, `"摸摸头充满电!"`, `"随时待命哦~"`）。
2. **连戳傲娇彩蛋（Combo Angry）**：
   - 记录最近点击时间戳：若 **8 秒内连续戳中宠物 ≥ 3 次**，本次触发 `NvaAnimKey::CLICK_ANGRY`（傲娇生气，one-shot 播满一圈），气泡弹傲娇台词（如 `"别老戳人家啦!"` / `"再戳要生气咯!"`），触发后清零连击计数。
3. **打盹戳醒专属惊醒（Wake Shock）**：
   - 若点按宠物时当前处于 `SLEEP` 或 `YAWN` 状态，不再播普通 `CLICK`，而是触发 `NvaAnimKey::WAKE_UP`（`打瞌睡被惊醒`，one-shot），配迷糊台词（`"呜哇! 没在偷懒!"` / `"唔...醒啦主人!"`）。

#### B2. 移动步伐与镜像规则升级（`src/ui/page_home.cpp` & `src/main.cpp`）
1. **镜像白名单 `spriteAllowsMirror(NvaAnimKey k)`**：
   - 精确对齐 `docs/pet-expressiveness-plan.md` §1 表的 `allowMirror` 列（凡带文字/代码屏/钱袋非对称道具的动作一律 `false`，纯肢体动作 `true`）。
2. **真·走路与小跑动作**：
   - 新增 `bool pageHomeIsRunning()` 暴露 `s_runSpeed && pageHomeIsWalking()`。
   - 在 `main.cpp` 状态机中，当 `pageHomeIsWalking()` 为真时：若 `pageHomeIsRunning()` 则播 `NvaAnimKey::HAPPY_RUN`（开心小跑），否则播 `NvaAnimKey::CRAB_WALK`（螃蟹走路），替代以前走路时死板播 `THINKING` 的旧逻辑！
   - 微调 `drawStageContent` 的 `hop`：既然 `CRAB_WALK` 和 `HAPPY_RUN` 本身已有生动的脚步帧动画，将程序叠加的正弦 `hop` 幅度从 `5.0f` 温和收小到 `2.0f`，避免脚底过度悬空飘移。

#### B3. 智能状态机联动：工具感知 + 犯困哈欠 + 余额感知 + 巡游池（`src/main.cpp` & `src/ui/page_home.cpp`）
1. **工作态细分（`st.run > 0`）**：
   - 根据 `st.lastTool`（大小写不敏感子串匹配）自动切换工作姿势：
     - 含 `edit / write / patch / bash / terminal / cmd / create` → `NvaAnimKey::CODING`（写代码）
     - 含 `read / grep / glob / search / find / list / fetch` → `NvaAnimKey::ARCHIVE`（清点归档）
     - `st.lastTool[0] == '\0'`（刚开回合还在思考）→ `NvaAnimKey::THINKING`
     - 其余工具 → `NvaAnimKey::WORKING`
2. **犯困两段式过渡（`YAWN → SLEEP`）**：
   - 原逻辑闲置 `90s`（USB）/ `45s`（电池）直接跳 `SLEEP`。
   - 改为：闲置达到阈值前 `15s`（即 USB `75..90s`、电池 `30..45s`）先进入 `NvaAnimKey::YAWN`（哈欠连天），状态胶囊显示 `"犯困打哈欠..."`；满阈值后再进入 `NvaAnimKey::SLEEP`。
   - **舞台 Zzz 微粒子**：当 `nvaActiveState() == NvaAnimKey::SLEEP` 时，在诺瓦头顶斜上方用现有的 `fontAscii14()` 或轻量矢量圆点画缓慢上浮淡出的 `"z"` / `"Z"` 小呼吸特效（确保严格落在 `curX0..curX1, curY0..curY1` 脏矩形内，绝不残留拖影！）。
3. **本地巡游池 `bgAutoKey` 丰富化**：
   - 无 AI 剧本时，本地自动轮换池从单调的 `{THINKING, QUOTA, WAITING}` 扩充为按上下文挑选：
     - 若 DeepSeek 余额有效且 `< 15.0` 元（或任一订阅卡 `bars[0].pct >= 80`）→ 轮换池加入 `QUOTA_WORRY`（数金皱眉）；否则加入 `QUOTA`（钱袋满溢）；
     - 若今日 Token（`st.nTokenDays > 0 && st.tokenDays[st.nTokenDays-1].tokens >= 100000000ULL`）破亿 → 轮换池加入 `EAT_TOKEN`（吃 Token）；
     - 常驻加入 `HUM`（悠闲哼歌）、`BUBBLE`（吐泡泡）、`STRETCH`（伸懒腰）。
4. **Demo 胶囊全量切换**：
   - `drawStatusPill` 的 `kDemoTags[24]`、`pageHomeSetDemoOverride`、`pageHomeTap` 的 `PILL_RECT` 循环上限同步扩至 `0..23`，并在胶囊状态文案中补充 `CODING / YAWN` 等自然描述。

#### B4. 模拟器用例与文档同步
1. 在 `tools/sim/main.cpp` 中：
   - 确认现有 47 张基线在哪些页面因 `run=1, lastTool=""`（样本默认 `st.run=1, st.lastTool=""` 会从 `WORKING` 变成 `THINKING`，注意检查！）发生变化：若希望保持 `page_home` 默认展示 `WORKING` 或 `CODING`，可在模拟器或规则里核对清楚；新增 2~3 张展示新动作的模拟器用例（如 `home_coding`、`home_sleep_zzz`），运行 `bash tools/sim/run.sh` 并用肉眼/读图逐张检查 `tools/sim/out/page_home*.png` 无裁切、无脏矩形越界（可仿照 `page_tokens_local` 加一个 `page_home_stage_local` 脏矩形哨兵断言！），确认无误后更新 `tools/sim/baseline.txt`。
2. 同步更新 `docs/ARCHITECTURE.md`（将“9 个 `.nva` / 9 枚举 / 8MB”更新为 24 个 `.nva` 与 16MB 分区说明）及 `HANDOFF.md`。

### 验收闭环
1. 运行 `bash tools/check_all.sh` 必须 100% PASS。
2. 若本机能找到 PlatformIO（检查 `tools/pio.sh` 或全局 `pio`），跑一遍 `waveshare-esp32s3-touch-amoled-1-8` 编译确认零新增 warning；若开发板已插在 `/dev/cu.usbmodem*`，上传固件与 LittleFS（`uploadfs` + `upload`），用 `tools/usb_shot.py` 抓真机图并跑 `{"cmd":"stat"}` 确认首页 `fps >= 55`、`pushUs` 无劣化。
3. 使用 Agent 身份（`-c user.name="Gemini Hydrogen" -c user.email="hydrogen@agent.com"`）按中文提交规范提交。
```

---

## 4. 终审清单（Step 3 切回 Carbon Medium 验收时核对）

- [ ] `no_ota_16mb.csv` 仅绑定在 `1.8` 板环境，`spiffs` 分区名未改，偏移与大小 4KB 对齐且总和 `0x400000 + 0xC00000 = 0x1000000 (16MB)`。
- [ ] `NvaAnimKey`、`ANIM_NAMES`、`animNameToIdx`、`VALID_ANIMS`、`kDemoTags`、`spriteAllowsMirror` 六处索引 `0..23` 一一对应无错位。
- [ ] `nva.cpp` 仍只有 3 个常驻 PSRAM 包，`MAX_FRAME_PIXELS` 未溢出，切动作无帧间 XOR 花屏（切换时 `s_frameIdx=0` 首帧必为 keyframe `0x00`）。
- [ ] `drawStageContent` 中新增的 `Zzz` 特效完全包在 `curX0..curX1 / curY0..curY1` 内，擦除干净无黑底残影。
- [ ] `bash tools/check_all.sh` 全绿通过。
