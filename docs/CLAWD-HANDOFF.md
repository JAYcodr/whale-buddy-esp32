# 克码人生 · Clawd 新角色接入交接文档

> **用途**：承接上一轮（`0239e5f` 收口后）开启的新对话，继续将 **Clawd（`cc-clawd`）** 作为新角色接入 nova-buddy。
> **前置交接**：系统架构、协议与此前踩坑记录见 [HANDOFF.md](HANDOFF.md)、[ARCHITECTURE.md](docs/ARCHITECTURE.md) 与 [VOICE-AUDIO.md](docs/VOICE-AUDIO.md)。

---

## 1. 当前基线与环境状态

- **代码仓库**：本仓库（`main` 分支已推送至 `origin/main`，HEAD = `0239e5f` `[新增] AOD 优雅退场、长任务插曲循环、动作折返与 TIDE 细节打磨`）。
  - 注：工作区另有一个未跟踪目录 `.workbuddy/`，提交时不要误加入暂存区。
- **硬件状态**：Waveshare ESP32-S3-Touch-AMOLED-1.8（deviceId `D93468`，串口 `/dev/cu.usbmodem21101`）已烧录 `0239e5f` 最新固件并正常运行。
- **上轮已闭环的问题**（无需重复返工）：
  1. `RetireMachine` 优雅退场与 `choreo`/动作池断点书签；
  2. 长任务期间穿插工作小插曲、跑步/蟹步触边折返；
  3. AOD 固定展示 `NvaAnimKey::SLEEP` 局部呼吸循环（90s 实测 `heap`/`psram` 零泄漏）；
  4. TIDE 气泡脏区擦除与顶栏左右内缩 `12px`；
  5. Demo 动作预览收口为仅限串口 `{"cmd":"demo","anim":N}` 调试触发。
- **已验证的用户意向**：
  - **Clawd 语音/音效方案已拍板**：不使用女声语音包，直接使用**电子音**（固件现成支持 `hwTone()` 滑频方/正弦电子音，或按 `VOICE-AUDIO.md` 规范离线生成 16kHz 电子音 `.adp` 音效包）。

---

## 2. Clawd 原始素材包盘点（已核验）

- **原始压缩包**（`克码人生-Clawd-cc-clawd-v1.0.0.zip`，`280,750` 字节）：素材未入库，需要重新转换时从上游 `alwinecor/clawd-stickers` 取。**勿把原始散图或大素材直接提交进仓库，注意 MIT 署名**。
- **元信息（来自包内 `pack.json`）**：
  - `schema`: `1` · `id`: `cc-clawd` · `name`: `克码人生 · Clawd`
  - `author`: `alwinecor` · `license`: `MIT` · `source`: `https://github.com/alwinecor/clawd-stickers`
  - 标准画布：`144×144`，脚底锚点 `anchor = { x: 72, baseline: 138 }`，`contentBox = { x: 27, y: 78, w: 90, h: 60 }`
  - `animations`：**44 套 PNG 精灵表**（全部为 `mode=P` 带透明通道索引色 PNG，压缩尺寸 204B–15KB）
- **视觉与工程质量评估**：
  - **画风与对比度（优点）**：方正像素风橘红 Claude 形象，配清晰道具（待办清单框、蓝色睡帽与 `Zzz`、咖啡杯、小黄鸭、`git push --force`、`main` 分支方块等），边缘硬朗无半透明白毛边，在 AOD 纯黑（`#000000`）与 TIDE 深海蓝（`#0A1A36`）底色上对比度很高。
  - **可直接使用的关键约束（需转码脚本适配）**：
    1. **缩放策略**：`pack.json` 虽标注 `pixelArt: false`，实测画面为纯像素风（见图块特征）。若做放大，必须使用 **`NEAREST`（最近邻）** 保住锐利像素边，不要沿用 Nova 的 `LANCZOS` 抗锯齿插值；同时 `NVA2` 最大单帧缓冲区 `MAX_FRAME_PIXELS = 220 * 224 = 49,280` 像素（[nva.cpp](src/anim/nva.cpp#L92)），且单帧宽度建议保持 **< 240px** 以继续走 [display.cpp](src/hw/display.cpp#L297-L325) 的 64KB 内部 DMA 快传路径。按 `1.5×` 整数/干净倍率将 `144×144` 阶段放大到 `216×216`（角色本体约 `90px` 身高，顶部道具最高 `216px`）刚好同时满足视觉体量与 `<220×224` 限制。
    2. **锚点对齐（非常关键）**：`pack.json` 每个动作裁切尺寸不同（`90×60` 到 `138×144`），靠 `box: { x, y, w, h }` 对齐 `anchor: (72, 138)`。而固件 `nvaDraw(c, centerX, bottomY)` 直接按帧底边 `y0 = bottomY - h`、水平中心 `x0 = centerX - w/2` 贴图（[nva.cpp](src/anim/nva.cpp#L547-L575)）。转换脚本**必须先在 `144×144` 阶段里按 `box.x / box.y` 复原位置，再按角色脚底线与水平中心统一对齐裁边**，否则 `todo` / `wake` / `forcepush` 会出现穿地或左右横跳。
    3. **可变帧时长适配**：Clawd 在 `pack.json` 里使用逐帧毫秒数与帧下标复用（例如 `idle` 只有 2 帧 `[5217ms, 183ms]`、`forcepush` 等复用帧下标、单帧 `60ms..540ms`），而 `NvaPack` 使用固定整型 `fps`（默认 `10 fps = 100ms`）。建议转换时按 `10 fps`（100ms 步长）重采样/展开关键帧；得益于 `NVA2` 帧间 XOR 差分 + zlib-9，连续相同帧只占极少字节且不会改变节奏太多。
    4. **调色板数量**：44 个精灵表中 42 个调色板 `<= 77` 色，仅 `banned_long`（106 色）与 `ultrathink`（90 色）超过 `convert_pet.py` 默认的 `DEFAULT_COLORS = 96`。`NvaPack` 格式本身支持 `uint8_t ncolors`（最高 `255` 色，索引 `0` 为透明），Clawd 转换工具可将上限设到 `128` 或直接使用源图调色板，尽量避免重新量化失真。

---

## 3. 硬件容量与多角色架构约束

### 3.1 Flash / LittleFS 空间余量（必须先验证）

- 1.8 寸主控板使用 [no_ota_16mb.csv](no_ota_16mb.csv)，`spiffs`（LittleFS）分区总大小为 `0xC00000` = **`12,582,912` 字节（12.0 MiB）**。
- 当前已占用空间（基于仓库 `assets/` 实测）：
  - `assets/pets/nova/*.nva`（24 个）：`10,906,364` 字节（约 `10.40 MiB`）
  - `assets/audio/*.adp`（17 个）：`368,635` 字节（约 `360 KiB`）
  - 合计：**`11,274,999` 字节（约 `10.75 MiB`）**，扣掉 LittleFS 元数据开销后剩余空间仅约 **~1.2 MiB**。
- **可行方案判断**：
  - **第一步先写离线转换脚本生成 Clawd 全套 `*.nva` 实测总大小**：由于 Clawd 是少色阶像素画，XOR 差分压缩率通常会远高于 Nova，24 个包大概率能压在 `1 MiB` 上下。
  - 若实测 `< ~1.0 MiB`：Nova（`/pets/nova`）与 Clawd（`/pets/clawd`）**可双角色同机共存**，直接在设置页切角色即可。
  - 若实测偏大塞不进同一 12 MiB 分区：可考虑两条路之一——① 进一步压缩 Clawd（减少展开帧率如 8fps / 合并静止帧），或 ② 在 [prep_data.sh](tools/prep_data.sh) 支持按 `--pet nova|clawd` 打包烧录对应角色包。

### 3.2 代码接入点地图（双角色 / 可切换）

| 模块 | 当前实现位置 | 接入 Clawd 需改内容 |
| --- | --- | --- |
| 动画格式与调度 | [nva.h](src/anim/nva.h)、[nva.cpp](src/anim/nva.cpp) | 当前 `ANIM_NAMES[24]` 为全局 24 动作契约；`s_rootDir` 默认 `/pets/nova`。若支持运行时切角色，需要封装 `nvaSwitchPet(const char* rootDir)` 安全暂停 `decodeTaskFn` 并重载常驻包（`IDLE/SUCCESS/ERROR`）及清空 `s_streamPack`，或切完触发软件重启 |
| 协议与宿主白名单 | [proto.cpp](src/net/proto.cpp#L236-L243)、`host-plugin/lib/ai.js` | **建议 Clawd 复用现有 0..23 动作表**（如 `crab_walk` / `bubble` / `eat_token` 等作为标准语义槽），避免改协议与破环 24 项一致性单测；若新增专属语义再四端同步 |
| 设置项与持久化 | [settings.h](src/model/settings.h)、[settings.cpp](src/model/settings.cpp)、[page_settings.cpp](src/ui/page_settings.cpp) | 可在 `NovaSettings` 参考 `uiTheme`（NVS key `theme`）模式增加 `petRole`（0=Nova, 1=Clawd），同步更新 `tools/test_settings_model.cpp` |
| 阶段绘制与交互 | [page_home.cpp](src/ui/page_home.cpp)、[main.cpp](src/main.cpp#L2360) | 按当前角色区分文案/气泡台词（Nova 软萌台词 vs Clawd 程序员极客台词，如 `passed_long` / `git push --force` 梗）以及角色是否允许 `flipX` 等属性 |
| 电子音反馈 | [sfx.cpp](src/audio/sfx.cpp)、[audio.cpp](src/hw/audio.cpp#L373-L400) | 当前已具备阻塞队列式多段滑频电子音 `hwTone(f0, f1, durMs, vol)`，Clawd 的唤醒/点击/生气/工作完成可组合成 2–4 音符的 8-bit chiptune 旋律序列，几乎零 Flash 占用 |
| 资产转换流水线 | [convert_pet.py](tools/assets/convert_pet.py)、[prep_data.sh](tools/prep_data.sh) | 新增 `tools/assets/convert_clawd.py` 读取 `pack.json` + PNG 精灵表导出 `assets/pets/clawd/*.nva` 与拼图预览图供人眼验收 |
| 模拟器回归 | [tools/sim/main.cpp](tools/sim/main.cpp) | 若支持运行期切角色，补 `Clawd` 帧绘制与无越界断言 |

---

## 4. Clawd 44 套素材 → 24 动作槽位建议映射

Clawd 原包有 `44` 套动画，固件标准槽位为 `24` 项（对应 [nva.h](src/anim/nva.h#L5-L31)）：

| 索引 & 槽位名 | Nova 原义 | 建议映射 Clawd 动画（可微调） | 备注 / 备选素材 |
| --- | --- | --- | --- |
| `0 idle` | 待机 | `idle` | 眨眼呼吸（2 帧插值成循环） |
| `1 thinking` | 思考 | `thinking` | 备选：`ultrathink`（高算力/深思） |
| `2 working` | 忙碌工作 | `todo` | 备选：`team2` / `team3` / `subagents`（多会话并行） |
| `3 waiting` | 等待审批 | `proceed` | 提示推进/等待确认 |
| `4 success` | 任务成功 | `passed_long` | 备选：`lgtm`、`works` |
| `5 error` | 执行异常 | `error` | 备选：`bug`、`notfound` |
| `6 click` | 轻戳挥手 | `hello` | 原包 `reactions.tapLeft` |
| `7 sleep` | 熟睡 | `sleep` | 带蓝色睡帽 + `Zzz`，完美契合 AOD 循环 |
| `8 quota` | 额度充足 | `lgtm` / `coffee` | 备选：`works` |
| `9 click_happy` | 开心反应 | `right` / `works` | 原包 `reactions.tapRight` |
| `10 click_shy` | 害羞反应 | `duck` | 小黄鸭互动，备选：`cherry` |
| `11 click_angry` | 连续戳生气 | `rmrf` | 备选：`banned_long`、`forcepush` |
| `12 click_giggle` | 挠痒笑 | `detached_long` | 原包 `reactions.tickle2` |
| `13 crab_walk` | 横行巡逻 | `roam` | 行走动画，可配边缘折返 |
| `14 happy_run` | 快乐奔跑 | `clone` / `roam` | 备选：`roam` 配二倍速或 `deploy` |
| `15 coding` | 敲代码 | `simple` / `forcepush` | 备选：`loop`、`deploy` |
| `16 archive` | 清点归档 | `compact` | 备选：`stash`、`clear` |
| `17 stretch` | 伸懒腰/收工 | `offwork` | 备选：`bye_long` |
| `18 yawn` | 犯困打哈欠 | `overtime` | 加班疲惫状态，退场过渡 |
| `19 wake_up` | 惊醒 | `wake` | 原包 `states.waking` |
| `20 hum` | 闲逛哼歌 | `coffee` | 端咖啡摸鱼 |
| `21 eat_token` | 吃 Token | `overflow` | 备选：`leak` |
| `22 bubble` | 摸鱼气泡 | `fish` | 备选：`duck`、`cherry` |
| `23 quota_worry` | 额度焦虑 | `deadline` | 备选：`bug`、`banned_long` |

---

## 5. 下个对话推荐推进顺序

1. **先把资产转换与视觉预览跑通**（零改动固件即可先验视觉与体积）：
   - 解压原始素材包，核对各动画 `box` 与脚底基准对齐策略；
   - 写 `tools/assets/convert_clawd.py` 生成 24 个 `.nva` + 预览拼图，先**截图发用户拍板**缩放倍数（1.0× vs 1.5×）与 24 个动作映射，并实测总字节数能否与 Nova 共存进 12 MiB LittleFS。
2. **确认交互/主题/电子音方案**：
   - 和用户敲定：双角色切换方式（设置页开关 `Nova / Clawd` vs 编译打包切换）、Clawd 专属气泡文案风格、以及电子音（多段 chiptune `hwTone` 序列 vs `.adp` 音效）。
3. **落代码 + 模拟器 + 真机验证**：
   - 先用本地 `tools/sim/run.sh` 验证 24 动作渲染与对齐，再 `uploadfs` + `upload` 上机实测。

---

## 6. 顺带备忘（调试工具细节）

- [tools/usb_shot.py](tools/usb_shot.py) 当前在 macOS 上开关串口会触发 `rst:0x15` 硬件复位，且固件 `sendUsbScreenshot()` 未屏蔽 Core 0 串口日志；后续真机验收截图若高频使用，可先顺手修一下这两个点再采图，免得误判成设备重启或出现错色条纹。
- 在 [src/hw/power.cpp](src/hw/power.cpp) `BOOT` 黑匣子事件里暂无 `esp_reset_reason()` 字段，如有需要可顺带补上，便于区分硬件复位与软件异常。
