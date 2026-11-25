# AGENTS.md — AI 与新接手者的入口

Nova Buddy = ESP32-S3（368×448 AMOLED）桌面副屏 + DSH 宿主插件。设备管显示与交互，Mac 管会话、凭据与出网，两者只通过 MQTT 通信。**改代码前先读 `docs/ARCHITECTURE.md`**，它写的是现在的代码，不是设想。

## 先读哪份

| 文档 | 什么时候读 | 里面有什么 |
|---|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | **改代码前必读** | 代码地图、任务与资源归属、MQTT 契约、页面/电源状态机、不变量与坑、影响地图 |
| [`docs/NOVA-BUDDY-PLAN.md`](docs/NOVA-BUDDY-PLAN.md) | 要知道「为什么这么设计」、改需求 | 需求、方案取舍、协议沿革 |
| [`HANDOFF.md`](HANDOFF.md) | 接着上一轮活干 | 最近实测数字、已知未修问题、验收路径 |
| [`docs/MOTION-CONTEXT.md`](docs/MOTION-CONTEXT.md) | 改动效曲线 / 时长 | 缓动体系与参数 |
| [`docs/动效细腻化任务书.md`](docs/动效细腻化任务书.md) | 做动效专项 | 逐条验收标准 |
| [`docs/pet-expressiveness-plan.md`](docs/pet-expressiveness-plan.md) | 动 24 个动作 / 16MB 分区 | 动作语义、镜像白名单、四端对齐表 |
| [`docs/VOICE-AUDIO.md`](docs/VOICE-AUDIO.md) | 换 / 加机内语音条 | 本地 TTS 生成、参考音切换与还原、响度、打包烧录 |
| [`docs/design/`](docs/design/) · [`docs/previews/`](docs/previews/) | 看设计稿 / 真机截图 | 语音看板原型、各页截图 |
| [`docs/upstream/`](docs/upstream/) | 只在追 fork 差异时 | fork 前基线，已废弃 |

## 常用命令

| 目的 | 命令 |
|---|---|
| 全量回归（改代码必跑） | `bash tools/check_all.sh` |
| 全页渲染 PNG | `bash tools/sim/run.sh` → `tools/sim/out/` |
| 像素基线对比 | `bash tools/sim/check.sh`（51 项） |
| 编译 / 烧录 | `bash tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8` |
| LittleFS 资源打包 | `bash tools/prep_data.sh` |
| 机内语音条 | `node tools/assets/gen_voice.mjs`（本机听书值守 → 本地 Qwen3-TTS），写完再 `bash tools/prep_data.sh` |
| 语音条解码 | `node tools/assets/nvad_decode.mjs <x.adp> <x.wav>`（核对内容/响度用） |
| 宿主插件测试 | `cd host-plugin && npm test` |
| 真机指标 | 串口 `{"cmd":"stat"}`；采样 `node tools/power_probe.mjs --id <ID> --minutes 30` |

## 硬规则

- **资源排他**：I2C 只 `task_bus`；canvas 只 `task_ui`；MQTT 句柄只 `task_net`；`.nva` 解码缓冲只 `task_decode`。
- **协议约定**：`err` 只能是字面量 `true`；`approval` / `choreo` / `tts` / `chat` / `mic_ack` 不 retain；审批同 `id` 必须幂等。
- **像素基线是验收口径**：改视觉就同步更新 `tools/sim/baseline.txt` 并在提交正文说明；没改视觉别动。
- 动作枚举五处同序、只许追加：`nva.h` / `nva.cpp:ANIM_NAMES` / `proto.cpp:kAnimNames` / `ai.js:VALID_ANIMS` / `page_home.cpp:kDemoTags`。
- 16MB 分区（`no_ota_16mb.csv`）只挂在 1.8 env，`spiffs` label 不能改名。
- `src/` 里不许出现裸 `368`（`check_all.sh` 会拦），屏参只放 `src/boards/`。
- 提交信息用 `[修复] / [新增] / [文档] / [工具]` 前缀 + 中文短句，正文写「原因 / 影响」。
