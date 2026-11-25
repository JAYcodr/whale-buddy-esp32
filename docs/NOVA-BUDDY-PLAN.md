# 诺瓦副屏（Nova Buddy）设计与需求书

> 硬件：Waveshare ESP32-S3-Touch-AMOLED-1.8（SH8601 + FT3168）
> 基线代码：`claude-desktop-buddy-esp32`（anthropics/claude-desktop-buddy 的 AMOLED 移植）
> 目标：把它从「Claude Code 外设」改造成「DSH 桌面副屏伙伴」
> 版本：v0.3 · 2026-11-21（v0.3 对齐画布 `368×448`、`WiFiManager`、单仓结构与交互电源细节，见 §1.4）

## 0. 相关仓库与路径

### 0.1 远程仓库

| 用途 | 地址 |
|---|---|
| 本项目（基线固件） | https://github.com/vthinkxie/claude-desktop-buddy-esp32 |
| 上游原型（M5StickC，BLE 协议来源） | https://github.com/anthropics/claude-desktop-buddy |
| 角色素材（114 个 webm + 表情包 + 提示词） | https://github.com/PC2005-cloud/dsh-pet |
| 低功耗 / WiFi 省电 / 配网参考实现 | https://github.com/78/xiaozhi-esp32 |
| 配网组件 `78/esp-wifi-connect` | https://github.com/78/esp-wifi-connect |
| 显示驱动库 Arduino_GFX | https://github.com/moononournation/Arduino_GFX |
| LittleFS 组件（查阅 subtype 行为） | https://github.com/joltwallet/esp_littlefs |
| 构建平台 pioarduino | https://github.com/pioarduino/platform-espressif32 |

### 0.2 本机绝对路径

**工作仓库（本项目所在目录）**

```
/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/claude-desktop-buddy-esp32
```

**同目录下的参考仓库**

| 内容 | 绝对路径 | 说明 |
|---|---|---|
| 角色素材源 | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/dsh-pet` | 114 个 webm 在 `dsh-pet/assets/webm/` |
| QQ 宠物逆向（仅状态模型参考） | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/qqpet_automation` | 素材是 SWF，风格不匹配 |
| 素材转换实验脚本 | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/nova-assets-test` | `final.py` 是参数扫描用的原型（绝对路径 `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/nova-assets-test/final.py`） |

**硬件资料与驱动补丁来源**

| 内容 | 绝对路径 | 说明 |
|---|---|---|
| 板子官方 demo + 引脚文档 | `/Volumes/hamelin-ssd/embedding/ESP32-S3-Touch-AMOLED-1.8` | `docs/pinout.md` 是引脚事实来源（绝对路径 `/Volumes/hamelin-ssd/embedding/ESP32-S3-Touch-AMOLED-1.8/docs/pinout.md`） |
| **SH8601 窄列补丁**（本仓库基础上必须同步） | `/Volumes/hamelin-ssd/embedding/ESP32-S3-Touch-AMOLED-1.8/lib/GFX_Library_for_Arduino/src/display/Arduino_SH8601.cpp` | 第 51–104 行，见 §2.2 |

**xiaozhi 参考实现（低功耗 / WiFi PS / 配网）**

| 内容 | 绝对路径 |
|---|---|
| 完整源码（origin/main 已拉取） | `/Volumes/hamelin-ssd/embedding/xiaozhi-esp32` |
| `PowerSaveTimer` | `/Volumes/hamelin-ssd/embedding/xiaozhi-esp32/main/boards/common/power_save_timer.cc` |
| `PowerSaveLevel` 三档状态机 | `/Volumes/hamelin-ssd/embedding/xiaozhi-esp32/main/application.cc`（`:315` `:505` `:513` `:721` `:834` `:370` `:381` `:994` `:1010`） |
| WiFi PS 档位实现 | `/Volumes/hamelin-ssd/embedding/xiaozhi-esp32/managed_components/78__esp-wifi-connect/wifi_station.cc`（`SetPowerSaveLevel` 在 `:282-299`，`listen_interval` 在 `:233`） |

**工具链**

| 内容 | 绝对路径 |
|---|---|
| PlatformIO Core 6.1.19 | `/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio` |
| ESP-IDF（四套） | `/Volumes/hamelin-ssd/01-项目工程/devtools/.espressif` |

**角色形象参考图**

```
/Users/hamelin/dsh-plugin-feishu-im/assets/whale-maid/v3/00-master-turnaround.png
```

**本项目新增的脚本与产物**

| 内容 | 相对路径 | 绝对路径 |
|---|---|---|
| 素材转换脚本 | `tools/assets/convert_pet.py` | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/claude-desktop-buddy-esp32/tools/assets/convert_pet.py` |
| PlatformIO 包装脚本 | `tools/pio.sh` | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/claude-desktop-buddy-esp32/tools/pio.sh` |
| 分区表（已改） | `no_ota_8mb.csv` | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/claude-desktop-buddy-esp32/no_ota_8mb.csv` |
| 角色素材产物 | `assets/pets/nova/*.nva` | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/claude-desktop-buddy-esp32/assets/pets/nova/` |

### 0.3 数据源

| 源 | 地址 |
|---|---|
| DeepSeek 余额 | `https://api.deepseek.com/user/balance` |
| OpenCode Go 用量 | `https://opencode.ai/zen/go/v1/usage` |
| OpenDesign 额度 | `https://amr-api.open-design.ai/api/v1/workspaces/current`<br>`https://amr-api.open-design.ai/api/v1/workspaces/{id}/billing/coding-plan-usage` |
| ModelDial 排行（CC BY 4.0） | `https://modeldial.com/api/v1/radar/latest.json` |

---

## 1. 需求

### 1.1 六条主线

| # | 需求 | 原文 |
|---|---|---|
| R1 | 显示订阅额度 | DeepSeek 官方 API、OpenCode Go、OpenDesign |
| R2 | 显示模型排行 | 数据源 modeldial.com（真实开发者实测，非刷榜） |
| R3 | 设计可爱形象 | 需要一张会动的角色 |
| R4 | 支持 DSH 事件审批 | 在设备上直接批准/拒绝工具调用 |
| R5 | 30–60fps + 电源管理 | 不能为动效牺牲续航 |
| R6 | 交互与音效 | 手势、转场、提示音 |

### 1.2 已明确排除

- ~~链路固定 BLE~~ **已推翻**。原判断「BLE 是唯一免改硬件中断约束的选择」是错的：TCA9554 的约束只影响**本地总线外设**（AXP2101 / PCF85063 / QMI8658）无法主动通知 CPU，BLE 和 WiFi 的射频中断都不经 TCA9554，两种链路同样不受限。结论改为 **WiFi + MQTT**，理由见 §3.0。
- 不重写 `src/hw/` 的驱动层和板级定义（已跑通）。
- 不做 OTA、不多设备并发、不做网页配置面板。
- 不伪造 token 花费数字：DSH 没有可读账单，额度页只显示供应商真实值。

### 1.3 验收标准

1. 插电与电池均可运行；**主屏角色动画实测 ≥55fps**，整屏重绘 ≥30fps，静态页近零推送。
2. 三张额度卡在线更新；断网时显示缓存值 + 数据年龄。
3. 排行页数据与 modeldial.com 一致，含 CC BY 4.0 署名。
4. DSH 触发审批时设备 500ms 内弹出；设备点「批准」→ 工具真的执行；无人应答超时自动拒绝（默认 20 秒）。
5. 熄屏策略与现有电池/翻面/USB 逻辑一致，60fps 只在必要区域开启。
6. `pio run -e waveshare-esp32s3-touch-amoled-1-8` 零警告通过。

### 1.4 修订记录

v0.1 里有三条判断是错的，v0.2 全部推翻并给出依据；v0.3 在联调启动前收敛了画布分辨率、配网库、工程组织与交互策略：

| 版本 | 变更 | 依据 |
|---|---|---|
| v0.2 | 推翻「BLE 是唯一免改硬件中断约束的选择」→ 改 **WiFi + MQTT** | §3.0 |
| v0.2 | 推翻「分层骨骼 sprite 自绘」→ 复用 `dsh-pet` 转 `.nva` 帧序列 | §9.1 |
| v0.2 | 推翻「spiffs 1.9MB 不够」→ 改 `no_ota_8mb.csv` 扩至 4.94MB | §2.3 |
| **v0.3** | 画布由 `184×224 (×2)` 改回 **原生 `368×448`**（PSRAM 329KB），消除与 §10 UI 排版及 `.nva` 分辨率的矛盾 | §2 / §10 / §11.1 |
| **v0.3** | WiFi/MQTT 配网改用成熟 Arduino 库 **`tzapu/WiFiManager`**（替换纯 ESP-IDF 的 `78/esp-wifi-connect`），MQTT 优先使用成熟 Arduino 库；确立「有成熟库优先用库、不满足再改装」原则 | §5.3 |
| **v0.3** | Broker 定为 `192.168.31.15:1883`（局域网匿名访问）；Host 插件放在本仓库 `host-plugin/` 子目录单仓管理；设置页新增「默认主页（宠物/订阅/榜单）」选项 | §5.3 / §10.1 / §11.3 |

---

## 2. 硬件事实（已核实）

项目内相对路径均以 `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/claude-desktop-buddy-esp32` 为根。

| 项 | 值 | 来源（相对本仓库） |
|---|---|---|
| MCU | ESP32-S3R8，240MHz 双核，8MB OPI PSRAM，8MB flash | `README.md:24-28`（四板对照表） |
| 面板 | SH8601，368×448 AMOLED，QSPI 4 线，**无背光** | `src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h:52-53` |
| 内部画布 | **原生 368×448**（`BOARD_DISPLAY_SCALE=1`，PSRAM 329KB，v0.3 改） | `src/hw/display.cpp`；尺寸定义 `src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h` |
| 触摸 | FT3168 @ 0x38，INT = GPIO21（**全板唯一真中断**） | `src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h:32` |
| 电源管理 | AXP2101 @ 0x34，ALDO3 → 屏幕供电轨 | `src/hw/power.cpp:9-21` |
| RTC | PCF85063 @ 0x51 | `src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h:61-62` |
| IMU | QMI8658 @ 0x6A/0x6B（需扫两个地址） | `/Volumes/hamelin-ssd/embedding/ESP32-S3-Touch-AMOLED-1.8/docs/pinout.md` |
| 音频 | ES8311 codec + NS4150B 功放（PA_CTRL=GPIO46） | `src/hw/audio.cpp`、`src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h`（`PIN_PA_CTRL`） |
| IO 扩展 | TCA9554 @ 0x20，管 LCD/TP 复位 | `src/hw/expander.cpp` |
| 分区 | factory **3MB** + spiffs **4.94MB**（`no_ota_8mb.csv`，v0.2 已改） | §2.3 |
| 构建平台 | pioarduino `53.03.13` + Arduino 3.x | `platformio.ini:2` |

### 2.1 中断约束（关键坑）

除触摸外，**所有外设中断都挂在 TCA9554 上，而 TCA9554 的 INT 引脚根本没连到 ESP32**。

| 中断源 | 硬件路径 | 是否能触发 ESP32 中断 |
|---|---|---|
| AXP2101（电源键、充放电） | TCA9554 `EXIO5` | ❌ 只能 I2C 轮询 |
| PCF85063（RTC 闹钟） | TCA9554 `EXIO3` | ❌ |
| QMI8658（IMU INT1） | TCA9554 `EXIO6` | ❌ |
| TCA9554 自身 INT | **未连接** | ❌ |
| FT3168（触摸） | GPIO21 直连 | ✅ |

**推论**：
- deep-sleep 不可行（唤醒源不足）。只能 light-sleep + 定时唤醒。
- 电源键必须靠 AXP 自己处理（PWRON 长按 4s 硬件断电）。
- 触摸是唯一能实时唤醒的输入；BLE 协议栈有自己的射频中断，不受此限制。

### 2.2 SH8601 窄列 CASET 缺陷

SH8601 的 QSPI 在 `w ≤ 16` 的窄窗口写入后会丢行或不显示。

本地仓库（Waveshare 官方 demo，含打过补丁的 GFX 库）：
`/Volumes/hamelin-ssd/embedding/ESP32-S3-Touch-AMOLED-1.8`

远程：`http://192.168.31.15:3011/hamelin/ESP32-S3-Touch-AMOLED-1.8`（自建 Gitea）
上游 GFX 库：https://github.com/moononournation/Arduino_GFX（**注意：上游没有这个补丁**）

补丁文件：`/Volumes/hamelin-ssd/embedding/ESP32-S3-Touch-AMOLED-1.8/lib/GFX_Library_for_Arduino/src/display/Arduino_SH8601.cpp:51-104`

已有补丁：
- `w == 1` → 逐像素 CASET
- `w <= 16` → 逐行 CASET
- `w > 16` → 原生 writeRepeat

**后果**：`drawFastVLine ×10` 只有 6.1 fps（README benchmark）。新 UI 必须避免窄竖线绘制。

**策略**：坚持 canvas 合成 + 整帧大窗口推送（现已如此），补丁从本地仓库同步到本项目。

### 2.3 分区扩容（v0.2 决策：方案 B）

角色素材 3.79 MB，原 spiffs 只有 1.9 MB。两条路：

| 方案 | 做法 | 代价 |
|---|---|---|
| A. microSD | SPI 模式（**不是 SDIO**），MOSI=GPIO1 / SCK=GPIO2 / MISO=GPIO3 / CS=EXIO7 | 要写 SD 驱动 + FATFS 约 200 行，且 SD 卡读取速度不确定 |
| **B. 扩分区** ✅ | 改 `no_ota_8mb.csv` 一行 | 无。本来就不做 OTA，固件 1.483MB 用不满 3MB 的 factory |

**用户拍板（m00679）：「先做B因为A可能还有一个SD卡速度的问题」**

改后的 `no_ota_8mb.csv`：

```
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x6000
phy_init,  data, phy,      0xf000,   0x1000
factory,   app,  factory,  0x10000,  0x300000     ← 原 0x600000
spiffs,    data, spiffs,   0x310000, 0x4F0000     ← 原 0x610000 / 0x1F0000
```

**为什么是 3MB 而不是 2MB**：固件实测 1,555,632 B = 1.483 MB。2MB 分区占 74.2%，只剩 529 KB 余量，加 WiFi 栈（约 +700 KB flash）会爆。3MB 下占 49.4%，余量充足。

**构建产物核对**（解析 `.pio/build/.../partitions.bin`）：

```
factory  off=0x010000 size=0x300000 (3.00MB)
spiffs   off=0x310000 size=0x4F0000 (4.94MB)
```

`pio run -t buildfs` 生成 `littlefs.bin` = 5,177,344 B（4.94MB × 0.983，4096 对齐开销）。

**两个已核实的细节**：

1. **subtype 写 `spiffs` 也能挂 LittleFS**。Arduino-ESP32 的 `LittleFS.begin()` 默认 `partitionLabel = "spiffs"`（`/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio/packages/framework-arduinoespressif32/libraries/LittleFS/src/LittleFS.h:29`），走 label 查找而非 subtype 查找；`esp_littlefs` v1.22.1 的 label 分支用 `ESP_PARTITION_SUBTYPE_ANY` 匹配（`https://github.com/joltwallet/esp_littlefs/blob/v1.22.1/src/esp_littlefs.c#L1435-L1440`），所以 subtype 是 0x82 照样能挂载。
2. **PlatformIO 的文件系统分区检测按「最后一个 FS 分区」取**（pioarduino builder `main.py` 的 fallback，`https://github.com/pioarduino/platform-espressif32/blob/develop/builder/main.py#L462-L470`），也支持 `board_build.filesystem_partition` 显式指定 label。当前 CSV 只有 spiffs 一个数据分区，两种方式结果一致。

**烧录注意**：改分区表必须 `pio run -t erase` 或 `esptool erase_flash` 后重烧，否则旧分区表残留会导致挂载失败。素材需重新 `pio run -t uploadfs`。

---

## 3. 架构：Mac 做事实，ESP32 做感觉

```
┌──────────────────────────────────────────────┐
│  ESP32-S3 (368×448)                          │
│  ┌────────────────────────────────────────┐  │
│  │ ui      唯一持有 canvas，deadline 调度  │  │
│  │ bus     唯一访问 I2C，20ms 批量读       │  │
│  │ net     WiFi + MQTT 客户端，事件驱动    │  │
│  │ audio   音效队列                        │  │
│  └────────────────────────────────────────┘  │
│  本地渲染 / 动画 / 页面 / 手势 / 音效合成     │
│  最后一帧缓存 + 离线指示                      │
└──────────────────┬───────────────────────────┘
                   │ MQTT over TCP · JSON · <400 B/s
                   │ QoS1 + retain
┌──────────────────┴───────────────────────────┐
│  Mosquitto broker（局域网）                   │
│  · retain 保存最后一份 state / quota / rank   │
│  · 设备离线时的消息由 broker 兜住              │
└──────────────────┬───────────────────────────┘
                   │ MQTT publish / subscribe
┌──────────────────┴───────────────────────────┐
│  DSH Host 插件 @local/nova-buddy             │
│  · approval/request answerer                 │
│  · agent/status + tool/call 聚合             │
│  · 出网抓取：DeepSeek / OpenCode / Design    │
│  · ModelDial 榜单抓取 + 6h 缓存              │
│  · 决定翻译（accept → allowed-once）         │
│  · 超时拒绝                                   │
└──────────────────────────────────────────────┘
```

### 3.0 为什么是 WiFi + MQTT 而不是 BLE（v0.2 修订）

v0.1 写「BLE 是唯一免改硬件中断约束的选择」，**这个推理不成立**。TCA9554 的约束只说明**本地总线上的外设**（AXP2101 / PCF85063 / QMI8658）没法主动通知 CPU；BLE 和 WiFi 的射频中断都由各自的协议栈处理，都不经过 TCA9554。两者同样不受那条约束影响，所以它根本不是区分依据。

真实取舍：

| 维度 | BLE NUS | WiFi + MQTT |
|---|---|---|
| 设备功耗 | 低一个数量级（射频占空比小） | 高，需长连接保活（可用 PS 档缓解，见 §8.5） |
| 离线语义 | 宿主自己维护「设备在不在」，重连窗口要补偿状态 | **broker retain**：设备醒来 subscribe 立刻拿到最新值 |
| Mac 端组件 | 要写 BLE 桥进程（bleak） | 插件直连 broker，少一层 |
| 延迟 | 7.5–22.5ms（连接间隔） | 局域网 10–50ms，审批够用 |
| 配网 | 不需要 | 需要 SSID/密码保存与配网流程 |
| 额外组件 | 无 | 一个 broker（mosquitto，单容器几十 MB） |
| 固件开销 | BLE 栈已有 | WiFi 栈约 +700KB flash、几十 KB RAM |

**结论：WiFi + MQTT。** 副屏只需要「最新状态」，retain 天生适配这个场景；设备熄屏、休眠、重启都不需要宿主做状态补偿。功率代价用 ESP-IDF 的 PS 档位处理（§8.5 小智的做法）。broker 本身不是门槛（用户原话：「broker很简单 不是什么门槛」）。

### 3.1 为什么这么切

| 放在 Mac | 理由 |
|---|---|
| DSH 内部事件 | 只有进程内拿得到（`ctx.approval` / `ctx.agents`） |
| 凭据、cookie、网络 | 设备不碰网；密钥永不上设备 |
| 数据裁剪排序 | 少传 = 少带宽 + 省 CPU |
| 审批语汇翻译 | `allowed-once` / `rejected` 是 DSH 概念 |
| 持久缓存 | 设备重启就丢 |

| 放在 ESP32 | 理由 |
|---|---|
| 渲染 + 60fps 动效 | 传像素流不现实 |
| 脏矩形 / 帧调度 | 贴硬件 |
| 页面布局与排版 | 逐帧跑的东西不能等网络 |
| 手势 / 转场 / 状态机 | 延迟必须 < 16ms |
| 音效合成 | 本地 I2S，不传音频 |
| 倒计时、RTC 走时 | Mac 只推一次 epoch+tz |
| 亮度 / 熄屏 / 焚屏缓解 | 电源状态在硬件上 |

---

## 4. 数据源（全部实测过）

| 源 | 端点 | 认证 | 刷新 | 状态 |
|---|---|---|---|---|
| DeepSeek 余额 | `GET api.deepseek.com/user/balance` | `DEEPSEEK_API_KEY` | 5min | ✅ 实测返回 `CNY 77.10` |
| OpenCode Go 用量 | `GET opencode.ai/zen/go/v1/usage` | `~/.local/share/opencode/auth.json` → `opencode-go.key` | 5min | ✅ 实测 weekly 73% / monthly 66% |
| OpenDesign 额度 | `amr-api.open-design.ai` 两个 GET | 会话 cookie（`__Secure-better-auth.session_token`） | 10min | ⚠️ API key 403，只能 cookie |
| ModelDial 排行 | `GET modeldial.com/api/v1/radar/latest.json` | 无 | 6h | ✅ CC BY 4.0，返回 117 行 |

### 4.1 OpenDesign 的处理

网页会话只能通过 cookie 认证（API key 实测 403）。插件内置两个入口：
- `nova-buddy auth opendesign --paste` → 从浏览器复制 Cookie 头粘贴，存 `~/.dsh/nova-buddy/opendesign.cookie`（600 权限）
- `nova-buddy auth opendesign --from-chrome` → 弹一次 macOS Keychain 授权读 Chrome cookie 库

两个端点（需 `X-Vela-Workspace-Id` 头，取自第一个请求）：
- `https://amr-api.open-design.ai/api/v1/workspaces/current`
- `https://amr-api.open-design.ai/api/v1/workspaces/{id}/billing/coding-plan-usage`

**不授权就那张卡显示「未连接」，不影响其它功能。**

### 4.2 ModelDial 的取舍

只取前 8 行，字段裁剪为：`rank / model / effort / score / backend / frontend / knowledge`。不传耗时、费用、route——屏幕装不下。底部强制显示 `ModelDial · CC BY 4.0`。

---

## 5. 设备协议（MQTT）

主题树（`nova/` 前缀，设备 clientId = MAC 后 6 位）：

| 主题 | 方向 | QoS | retain | 载荷 |
|---|---|---|---|---|
| `nova/<id>/state` | 宿主 → 设备 | 1 | ✅ | 会话/工具/上下文 |
| `nova/<id>/quota` | 宿主 → 设备 | 1 | ✅ | 三张额度卡 |
| `nova/<id>/rank` | 宿主 → 设备 | 1 | ✅ | 排行前 8 |
| `nova/<id>/tokens` | 宿主 → 设备 | 1 | ✅ | 累计 token / 近 14 天日用量 |
| `nova/<id>/approval` | 宿主 → 设备 | **1** | ❌ | 审批请求（不能 retain，否则重连后重复弹） |
| `nova/<id>/approval_end` | 宿主 → 设备 | 1 | ❌ | 审批已撤（浏览器端先答了） |
| `nova/<id>/time` | 宿主 → 设备 | 0 | ✅ | epoch + tz |
| `nova/<id>/up` | 设备 → 宿主 | 1 | ❌ | 决定 / 手势 / 自报 |
| `nova/<id>/hello` | 设备 → 宿主 | 1 | ❌ | LWT 的 `offline` 也走这里 |

**设备 LWT（last will）**：`nova/<id>/hello` = `{"online":false}`，QoS1 retain。断链时 broker 自动发布，宿主立刻知道设备掉线，不用等超时。

**为什么 approval 不 retain**：retain 会在设备重连时重放一条已经过期的审批请求，用户会看到早就该消失的弹窗。审批走 QoS1 在线投递 + TTL 自毁。

### 5.1 上行（设备 → 宿主）

```json
{"hello":{"fw":"1.0","w":368,"h":448,"fps":60,"ip":"192.168.31.x"}}
{"evt":"decision","id":"ap_7f3","accept":true}
{"evt":"gesture","g":"tap","x":120,"y":300}
{"evt":"ack","what":"state"}
{"evt":"stat","fps":58,"drop":3,"heap":182400,"rssi":-52}
```

### 5.2 下行（宿主 → 设备）

```json
{"t":"time","epoch":1763700000,"tz":28800}

{"t":"state","run":1,"wait":0,"sess":3,"msg":"bash: pio run","ctx":{"pct":42}}

{"t":"quota","cards":[
  {"id":"ds","name":"DeepSeek","kind":"bal","val":77.10,"cur":"CNY","age":12},
  {"id":"oc","name":"OpenCode Go","kind":"win","bars":[[73,"w",7200],[66,"m",864000]]},
  {"id":"od","name":"OpenDesign","kind":"na","why":"no-session"}]}

{"t":"rank","src":"ModelDial","lic":"CC BY 4.0","at":1763700000,
 "rows":[{"r":1,"m":"gpt-6.2-astra","e":"xhigh","s":95.5,"b":94,"f":98,"k":95}]}

{"t":"approval","id":"ap_7f3","tool":"Bash","reason":"rm -rf /tmp/x","ttl":20000}
{"t":"approval_end","id":"ap_7f3"}
```

体积不变：`rank` 8 行 ≈ 700 B，`quota` ≈ 260 B，`state` ≈ 180 B。MQTT 单包上限远大于 BLE MTU 的 185 B，**不需要再分片**。

### 5.3 配网与 Broker

使用成熟 Arduino 库 `tzapu/WiFiManager`（配合自定义 `WiFiManagerParameter`）：

- 首次开机无凭据 → 开 AP 热点 `Nova-XXXX` 并弹 Captive Portal，手机连上填 SSID/密码及 MQTT Broker 地址/端口（默认 `192.168.31.15:1883`，局域网匿名访问）
- 有凭据 → 直接连，失败则回退 AP 模式；也可在设置页触发重新配网
- WiFi 凭据与 Broker 参数分别存 NVS，掉电不丢

MQTT 客户端优先采用成熟 Arduino MQTT 库，支持 QoS1 / retain / LWT 优先，不满足时再评估改装。

---

## 6. 同步频率

| 通道 | 触发 | 上限 | 体积 |
|---|---|---|---|
| 审批 | DSH 事件即刻 | 延迟目标 500ms | ~200 B |
| 状态 | 事件驱动 + 400ms 心跳兜底 | 无变化不发，≤2.5 次/秒 | ~180 B |
| 额度 | DeepSeek 5min / OpenCode 5min / Design 10min | 页面激活时才推设备 | ~260 B |
| 排行 | 6h | — | ~700 B |
| 时间 | 连接时推一次 | 设备 PCF85063 自走 | 30 B |
| 设备自报 | 每 30s | — | ~80 B |

**稳态 < 400 B/s**，峰值 < 1.5 KB。

---

## 7. 异常处理

原则：谁出问题降谁的级，不搞一刀切。

| 故障 | 检测 | 动作 | 屏幕表现 |
|---|---|---|---|
| WiFi 断链 | broker LWT 或 15s 无包 | 保留最后一帧，指数重连 2/4/8/16/30s；**重连后 subscribe 自动拿到 retain 的最新状态**，不需要宿主补推 | 顶部红点 + `offline 12s` |
| Broker 挂了 | TCP 连接失败 | 设备端指数退避重连；宿主端同理 | `broker down` |
| DSH 进程没了 | 插件退出 → broker 上 `nova/<id>/state` 超过 60s 未更新 | 停推状态，继续推额度 | `DSH not running` |
| 某额度源 401/403 | HTTP 状态码 | 该卡置灰 + `reauth needed` | 只影响那张卡 |
| 某额度源超时 | 3 次重试后放弃 | 用缓存 + `ageSec` | 数值正常，`12m ago` |
| ModelDial 抓不到 | — | 用 6h 缓存 | 照常显示（分数本来就不变） |
| 审批中途掉线 | `req.signal` abort | 插件走 `next()` → 浏览器弹 | 设备清屏 |
| 设备时间错乱 | `hello` 时偏差 > 60s | 强推 `time` | 时间跳正 |
| 掉帧超阈 | `drop > 30/10s` | 60→30 降档并记日志 | 角落显示 `30fps` |

**审批是唯一不能被降级的通道。** 断线时立刻切「等待重连」，不把倒计时环停在某个数字上骗人。

---

## 8. 电源管理

### 8.1 四档状态机

| 档 | 触发 | 帧率 | 面板 | 备注 |
|---|---|---|---|---|
| 活跃 | 屏亮 + 主屏/转场 + 5s 内有动效 | 60 fps（仅脏矩形） | 亮度 4/5 | |
| 静态 | 详情/额度/排行页，或主屏 10s 无变化 | 0 fps，变化时推一次 | 亮度 2 | |
| 熄屏 | 现有规则：电池普通 30s / 电池+时钟 5min / USB 不熄 | 0 fps | `displayOff()` | 触摸可唤醒 |
| 休眠 | 电池 + 熄屏 30min + 无 MQTT 连接 | — | 断电 | AXP PowerOff，默认关 |

沿用现成逻辑（`src/main.cpp:1396-1405`，常量在 `:118-119`）：USB 不熄屏、翻面即灭、每 5 分钟全屏重绘防焚屏。

### 8.2 参考实现（xiaozhi-esp32 `origin/main`）

本地源码：`/Volumes/hamelin-ssd/embedding/xiaozhi-esp32`（`origin` = https://github.com/78/xiaozhi-esp32）

读了最新代码，它的做法是：
- `PowerSaveTimer(-1, 60, 300)`：60s 降亮度到 20%，300s 直接 AXP 断电
- **不切 ALDO3，不用 deep-sleep**，只调面板 `SetPowerSaveMode` + 背光
- 插 USB 时不进省电流程（挂在 `discharging` 上）
- PWR 短按用独立任务 100ms 轮询 AXP IRQ 寄存器（绕开中断缺失）
- 少数板子用 `SleepTimer` 手动 `esp_light_sleep_start()` + 30s timer 唤醒

**采纳**：`esp_pm_configure(max 240 / min 80, light_sleep_enable=true)` + 面板 displayOff。
**不采纳**：300s 自动断电（副屏要常驻收审批）。

### 8.3 禁止

- 不切 ALDO3：屏幕断电后重初始化要走 9 步复位时序，唤醒到出画要一两百 ms，动效连续性没了。
- 不改 `setSysPowerDownVoltage`：保持 2.6V。改成 3.3V 会让大负载时 VSYS 一跌就硬关机 + 黑屏（`/Volumes/hamelin-ssd/embedding/ESP32-S3-Touch-AMOLED-1.8/AGENTS.md:108-113`）。

### 8.4 电量估计（0.4.0 已实装）

AXP2101 不暴露真实电池电流，库仑计（0xA4）也**没有为本项目的 600mAh 电池校准**，所以不能做电流积分。现在的做法（`src/model/battery_model.h`）：

- **OCV 曲线**：18 点分段线性，`4.20V→100%`、`3.35V→0%`；放电端电压补偿 `+50mV`（负载压降），CC 段补偿 `-70mV`（端电压 = OCV + IR）。
- **阶段机**：PRE/CC 用「电流 × 时间」模型（300mA / 600mAh ≈ 0.79%/min），CV 从 95 缓慢走到 99，DONE 锁到 cap；保养停充（LIMITED）时**锁定显示**，不让静置电压回落被读成耗电。
- **锚点**：DONE→100、`<3.4V` 持续 10s→0、开机首次无 USB 时用曲线直锚。
- **显示限速**：放电最多 1%/10s（回升 1%/60s），充电 1%/3s；电量变化的重绘限流为「5% 桶或 15s」。
- **上限 cap**：`limitPct` 传进模型，CV/DONE/CC 都不会越过保养上限（90% 上限就停在 90，不会先显示 100）。

参数与断言：`src/model/battery_model.h` + `tools/test_battery_model.cpp`（`bash tools/test_battery.sh`，998 项）。

**取舍**：仍然不做「预计可用/充满时间」——没有真实电流，估出来的数字比现状更不可信。60fps 与长续航的对立不变（见 §8.1），靠 `powerProfile` 与熄屏策略处理。

设备自报：`{"evt":"stat","fps":58,"drop":3,"heap":182400,"rssi":-52,"mv":3980,"pct":72,"chg":1,"stage":2,"tmp":33}`。

### 8.5 充电治理（0.4.0 已实装）

| 项 | 值 |
|---|---|
| 充电上限 | 90%（设置页 80/90/100；到上限停充，**拔插重新武装**） |
| 电流（温和） | 亮屏 200mA / 熄屏 300mA |
| 电流（自动，默认） | 亮屏 300mA / 熄屏 500mA |
| 电流（快速） | 500mA / 500mA |
| 温度分档（PMU die） | ≥49°C→200mA，≥53°C→100mA，≥59°C 停充，<44°C 恢复 |
| 硬件参数 | 4.2V 截止 / 预充 50mA / 终止 50mA / VBUS 限流 500mA / 库仑计 0x18 bit3 |

- **不设** `setLowBatShutdownThreshold()`：它按未校准的 gauge SOC 触发，可能在 3.7V 就把整机硬关。
- **不开** TS/NTC 的 JEITA：库默认 `disableTSPinMeasure()`，且阈值未在本板验证；温度用 die 温度，阈值偏保守。
- 停充只关电池充电（`0x18 bit1=0`），VBUS 继续供系统；真机需验证停充时设备不掉线。

### 8.6 WiFi 省电：抄小智的现成做法

参考 `78/esp-wifi-connect` 组件（https://github.com/78/esp-wifi-connect）+ xiaozhi 的 `PowerSaveLevel` 状态机。**没有自定义逻辑，只是把 ESP-IDF 两个参数调到合适档位。**

本地文件（xiaozhi 仓库内已 vendored）：
`/Volumes/hamelin-ssd/embedding/xiaozhi-esp32/managed_components/78__esp-wifi-connect/wifi_station.cc`

#### 两个寄存器级参数

```cpp
// wifi_station.cc:282-299
void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            ps_type = WIFI_PS_MAX_MODEM;   // 最大省电
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;   // 最小省电
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;        // 不省电
            break;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps_type));
}
```

**第二个参数更关键**：建连时固定 `wifi_config.sta.listen_interval = 10`（`/Volumes/hamelin-ssd/embedding/xiaozhi-esp32/managed_components/78__esp-wifi-connect/wifi_station.cc:233`）。IDF 默认是 3。`WIFI_PS_MAX_MODEM` 的实际行为就是按 `listen_interval` 拉长 beacon 接收间隔 —— 从 3 改成 10，唤醒频率降到约三分之一。

#### 小智的三档状态机

| 状态 | 档位 | 触发点（`/Volumes/hamelin-ssd/embedding/xiaozhi-esp32/main/application.cc`） |
|---|---|---|
| 激活完成、空闲 | `LOW_POWER` | `:315` |
| 开始连音频通道 | `PERFORMANCE` | `:721`、`:834`（注释：before connecting to reduce latency） |
| 音频通道已开 | `PERFORMANCE` | `:505` |
| 通道关闭 | `LOW_POWER` | `:513` |
| 下载资源 / OTA | `PERFORMANCE` | `:370`、`:994` |
| 下载完成 / 失败 | `LOW_POWER` | `:381`、`:1010` |

**原则**：平时 LOW_POWER，只在需要低延迟或高吞吐的事件发生前临时抬升，事件结束立刻回落。

#### 套到本项目

我们没有音频通道，绝大多数时间都不需要低延迟：

```
平时（无审批、无请求）      → WIFI_PS_MAX_MODEM + listen_interval=10
审批请求到达 / 用户点按钮   → WIFI_PS_NONE（临时抬升，保证 decision 回传不卡）
审批完成 / 超时             → 回落 MAX_MODEM
OTA / 素材上传             → PERFORMANCE
```

比小智更省，因为我们连音频那样的持续流都没有。切换本身是几毫秒的事，不需要重启 WiFi。

---

## 9. 角色设计

### 9.1 技术选型：帧序列 + 调色板 + zlib，不用分层骨骼也不用手写

**v0.1 说的「分层骨骼 sprite 自绘」撤回。** 理由：dsh-pet 已经有 114 个成品动画，是同一个角色（蓝紫长发 + 深蓝鳍耳 + 鲸尾 + 围裙小鲸鱼），而且它的状态机就是照 DSH 事件写的。重新画部件纯属浪费。

| 项 | 值 |
|---|---|
| 远程仓库 | https://github.com/PC2005-cloud/dsh-pet |
| 本地仓库 | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/dsh-pet` |
| 动画源目录 | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/dsh-pet/dsh-pet/assets/webm` |
| 状态机配置 | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/dsh-pet/dsh-pet/assets/config.jsonc` |
| 三视图 | `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/dsh-pet/dsh-pet/assets/main-character.png` |

素材本身不能直接上设备（VP9-alpha 解码器、640×360、55.2 MB），必须转格式。**为什么不用 RLE、不用帧间差分，数字在下面。**

#### 9.1.1 素材实测

| 项 | 值 |
|---|---|
| 数量 | 114 个 webm（VP9-alpha，`pix_fmt=yuv420p` + `TAG:ALPHA_MODE=1`） |
| 尺寸 | 640×360，24fps，各 10.042s（少数 4.9s） |
| 体积 | 共 55.2 MB，单文件 194 KB–1299 KB，中位 472 KB |
| 角色包围盒 | 222×271（在 640×360 里居中偏下） |
| 许可 | 代码 MIT；**素材 CC BY-NC-SA 4.0（署名·非商业·相同方式共享）**，原型「溟月」@上善无形（bilibili），DeepSeek 版 @ZipZipPipe（bilibili）；详见 https://github.com/PC2005-cloud/dsh-pet#许可 |

#### 9.1.2 压缩方案对比（4 秒 / 48 帧 / 170×208 实测）

| 方案 | 体积 | 结论 |
|---|---|---|
| RLE（整帧） | 2712 KB | ❌ 只压 1.7×。一帧有 **19,581 种颜色**，相邻像素灰度差 13.49 |
| RLE（帧间脏块） | 2695 KB | ❌ 几乎没省 |
| 4bit 打包 + zlib | 381 KB | 中游 |
| 关键帧（每 12 帧）+ 变化像素列表 | 309 KB | ❌ 比整帧还大（差分帧平均 2637 像素、压缩后 6.64 KB/帧） |
| **调色板索引 + zlib(deflate)** | **199 KB** | ✅ 采用 |

#### 9.1.3 参数扫描（4 秒，16 色，中值滤波开）

| 目标宽 | 实际尺寸 | 4s 体积 | 每帧 | 推算 10s |
|---|---|---|---|---|
| 96 | 96×117 | 123.9 KB | 2.58 KB | 310 KB |
| 112 | 112×137 | 159.2 KB | 3.32 KB | 398 KB |
| 128 | 128×156 | 198.7 KB | 4.14 KB | 497 KB |
| 144 | 144×176 | 236.1 KB | 4.92 KB | 590 KB |
| 170 | 170×208 | 312.9 KB | 6.52 KB | 782 KB |

调色板色数（128 宽固定）：16 色 425 KB / 24 色 489 KB / 32 色 545 KB（10 秒）。

#### 9.1.4 最终参数

**128×156 上限 / 32 色 / 3×3 中值滤波开 / 12fps / zlib-9**

- 选 32 色而不是 16：头发的蓝色渐变在高光处明显断层，多花 120 KB 换得值
- 选 12fps 而不是 24：原素材动态主要是呼吸和眨眼，12fps 够；帧数直接减半
- 中值滤波省 23%（258.9 → 198.7 KB @16 色），代价是线条略软，可接受

#### 9.1.5 已转出的 9 个核心动画

脚本 `tools/assets/convert_pet.py`，产物在 `assets/pets/nova/`：

| key | 源文件 | 尺寸 | 帧 | 体积 |
|---|---|---|---|---|
| `idle` | 待机呼吸休闲 | 128×145 | 121 | 545.3 KB |
| `thinking` | 工作状态-思考冒泡 | 128×143 | 121 | 508.8 KB |
| `working` | 工作状态-忙碌点按 | 128×155 | 121 | 485.1 KB |
| `waiting` | 工作状态-原地踱步张望 | 128×146 | 121 | 500.6 KB |
| `success` | 工作状态-雀跃庆祝 | 128×134 | 121 | 389.4 KB |
| `error` | 工作状态-垂头叹气冒汗 | 128×135 | 59 | 233.9 KB |
| `click` | 点击回应-元气挥手 | 128×168 | 121 | 612.3 KB |
| `sleep` | 睡觉-循环 | 128×128 | 121 | 347.7 KB |
| `quota` | 余额-钱袋满溢 | 128×82 | 121 | 168.8 KB |
| | | | | **3791.9 KB** |

`workStatus` 6 档语义来自 `/Volumes/hamelin-ssd/01-项目工程/开发/第三方fork/dsh-pet/dsh-pet/assets/config.jsonc`，和我们的 DSH 事件严格对应：

```
0 thinking（turn/start）      1 working（tool/call）      2 result（tool/result）
3 waiting（approval/asked）    4 success（turn/end completed）  5 error（turn/end error|max-tokens）
```

#### 9.1.6 `.nva` 包格式（自定义，25 字节头 + 帧数据）

```
magic  'NVA1'              4 B
w, h, nframes              u16 × 3
ncolors, fps               u8 × 2
palette                    RGB888 × ncolors
offsets                    u32 × (nframes+1)
blobs                      zlib-9 索引图，每帧 w*h 字节解压后
```

解码路径：`uncompress()` → 索引图 → 查调色板转 RGB565 → blit 到 canvas。
128×155×1 = 19,840 B/帧；ESP32-S3 的 `tinfl` 解压约 1–2 ms，12fps 下不是瓶颈（**未真机验证**）。

#### 9.1.7 分级加载

3791.9 KB 占 4.94 MB 分区的 75%，且 RAM 装不下 9 个全解。策略：

- **常驻**：`idle` + `success` + `error`（必要状态，约 1169 KB）
- **按需**：`thinking / working / waiting / click / sleep / quota`，从 LittleFS 流式读、只解当前帧
- 帧缓冲 128×155×1 B = 19.8 KB，双缓冲 40 KB，PSRAM 宽裕

#### 9.1.8 版权要求（必须落到 UI）

CC BY-NC-SA 4.0 要求**署名**。详情页固定一行：
```
角色素材 © PC2005-cloud/dsh-pet · CC BY-NC-SA 4.0 · 非商业
github.com/PC2005-cloud/dsh-pet
```
本项目为个人使用，不涉及商业化；若要开源，衍生素材也须 CC BY-NC-SA 4.0。

### 9.2 保留 GIF 通道

`characters/` 目录结构不变。加载 GIF 包时降级为「低帧率角色 + 60fps 特效层」，与 `.nva` 包共存。

### 9.3 形象来源（保留）

参考 `/Users/hamelin/dsh-plugin-feishu-im/assets/whale-maid/v3/00-master-turnaround.png`（七头身成年版），如需换角色再走「3 个互斥概念 → 选定 → 展开」流程。当前用 dsh-pet 的现成资产。

---

## 10. 界面与交互

368×448，安全内缩 8。

| 页 | 内容 | 动效 |
|---|---|---|
| 主屏 | 角色居中偏下、顶部状态条（时间/电量/连接）、下方 2 行状态、喷水粒子 | 60fps 呼吸+眨眼+尾摆；新事件「抬头」+ 短音 |
| 额度 | 三张卡竖向排列，每卡 120px：名称/主值/进度条/重置倒计时/数据年龄 | 数值滚动补间 300ms |
| 排行 | 前 8 行：名次/模型短名/总分/三根能力条；当前 DSH 模型高亮描边；底部署名 | 逐行淡入 40ms 错峰 |
| 审批 | 工具名大字 + reason 滚动 + 两个大按钮 + 倒计时环 + 「转电脑」提示 | 环 1s 步进，最后 3s 变红 + 滴答 |
| 详情 | 会话列表、上下文压力、堆、MAC、固件版本、触摸「设置」入口 | 无 |

### 10.1 手势与按键

- 左右循环滑：`宠物 ↔ 订阅 ↔ 榜单 ↔ 详情` 翻页（180ms 滑动转场）；详情页提供触摸「设置」按钮入口
- 设置页支持配置：默认主页（`宠物 / 订阅 / 榜单`）、电源档位（`自适应 / 长续航 / 灵动`）、音量与亮度
- 审批唤醒：息屏待机收到审批自动亮屏弹窗 + 3 音提示；朝下扣放（`napping`）时不亮屏直接回退电脑；单次点击「批准 / 拒绝 / 转电脑」立即生效
- 长按角色：切换 `.nva` / GIF 包模式
- Key1 短按：回到所设默认主页；长按：设置页
- PEK：熄屏

### 10.2 音效

复用 `hwBeep` 扩展为 `hwTone(f0, f1, durMs, vol)`（带包络与滑音）：

| 场景 | 音型 |
|---|---|
| 翻页 | 40ms 轻 chirp |
| 新事件 | 2 音上行 |
| 审批到达 | 3 音 |
| 批准 | 上扬 |
| 拒绝 | 下行 |
| 低电量 | 2 音下行 |

音量跟随现有 `settings().sound`。

---

## 11. 工程划分

### 11.1 固件侧改动

**保留不动**：`src/hw/*`（除 display 推送、audio `hwTone` 与 I2C 收口到 `task_bus`）、`src/boards/*`、GIF 通道（`src/character.*`）。

**HAL 加法**：
- `hwDisplayPushRect(x, y, w, h)`：原生 `368×448` 画布局部脏矩形直推（`x == 0 && w == 368` 零拷贝，其余情况按偶对齐 + `w >= 18` 打包进 PSRAM scratch 一次 `draw16bitRGBBitmap`）
- 原 `hwDisplayPush()` 改为全屏推送 `hwDisplayPushRect(0, 0, HW_W, HW_H)`，删掉 448 次逐行调用
- 预算：全屏 329,728 B ÷ 20 MB/s ≈ 16.5ms（@40MHz），@80MHz ≈ 8.3ms；局部角色脏矩形 128×156 ≈ 40 KB ≈ 1–2ms
- 先试 `begin(80000000)`（80MHz），花屏就退回 40MHz
- `hwTone(f0, f1, durMs, vol)`

**删除/替换（Claude 专属）**：
- 移除 `src/data.h`、`src/ble_bridge*`、`src/xfer.h` 与 18 个 ASCII `src/buddies/*`
- `src/stats.h` 里的 `appr/deny/vel/nap/lvl` → `frames/droppedFps/quotaAge`
- 广播名 `Claude-XXXX` → `Nova-XXXX`

**新增模块**：

| 文件 | 职责 |
|---|---|
| `src/net/wifi.cpp` | WiFi + Broker 配网（`tzapu/WiFiManager`）+ PS 档位切换 |
| `src/net/mqtt.cpp` | Arduino MQTT 客户端、主题订阅、LWT、retain 处理 |
| `src/net/proto.cpp` | JSON 编解码、协议版本 |
| `src/model/state.h` | 设备侧状态机 + 解析 |
| `src/ui/canvas.h` | 轻量 Canvas 抽象（ESP32 GFX / Mac 主机模拟器共用） |
| `src/ui/nav.cpp` | 手势/按键 → 页面状态机 + 转场 |
| `src/ui/page_home.cpp` | 主屏（宠物） |
| `src/ui/page_quota.cpp` | 额度 |
| `src/ui/page_rank.cpp` | 排行 |
| `src/ui/page_approval.cpp` | 审批 |
| `src/ui/page_detail.cpp` | 详情 + 设置入口 |
| `src/ui/page_settings.cpp` | 设置页（默认主页/电源档位/亮度/音量/重配网） |
| `src/anim/sprite.cpp` | `.nva` 解码 + 帧调度（见 §9.1） |
| `src/anim/anim.cpp` | deadline 调度、缓动、粒子 |
| `src/audio/sfx.cpp` | 音效序列 |

### 11.2 RTOS 任务划分

| 任务 | 核 | 优先级 | 周期 | 职责 |
|---|---|---|---|---|
| `task_ui` | Core 1 | 5 | deadline 16.6ms | 唯一持有 canvas。渲染、转场、手势、帧调度、推送 |
| `task_bus` | Core 0 | 6 | 20ms | 唯一访问 I2C。一次事务批量读 AXP+RTC+IMU+TCA9554，结果写快照 |
| `task_net` | Core 0 | 4 | 事件驱动 | MQTT 收包、JSON 解析、状态合并、WiFi 重连、PS 档位切换 |
| `task_decode` | Core 1 | 3 | 12fps | `.nva` zlib 解压 + 调色板转 RGB565，产出就绪帧供 `task_ui` 取用 |
| `task_audio` | 任意 | 5 | 事件驱动 | 复用现有 `beepTask` |

**四条硬规矩**：
1. I2C 只允许 `task_bus` 碰（六设备共享一条总线，且无中断）
2. canvas 只允许 `task_ui` 碰，其他任务一律投队列
3. `task_ui` 按 deadline 排程（`nextFrameAt += 16667µs`），不是固定 `delay(16)`
4. 解码放独立任务（低优先级），`task_ui` 取不到新帧就重复上一帧，**不让解压阻塞渲染**

现有固件只有一个 `beepTask`，UI 全在 `loop()` 里（`src/main.cpp:994`，460 行）。这 460 行必须拆。

### 11.3 Host 插件 `@local/nova-buddy`（位于 `host-plugin/`）

单仓放在 `nova-buddy/host-plugin/` 子目录，标准 DSH bundle：`package.json` + `cordis.patch.yml` + `index.js` + `locale/{en,zh}.json` + `icon.svg` + `docs/DETAIL.md`。

- `inject = ['approval','agents','sessions','tokenMeter']`
- 监听 `approval/request`：设备优先，超时自动拒绝
  - 设备离线（LWT 或 `nova/<id>/hello` 未收到）→ `next()`
  - 在线 → publish 到 `nova/<id>/approval`（QoS1，不 retain），等设备回 `nova/<id>/up`；默认 `fallbackMs=20000`，`prefer` 期间不调 `next()`，超时返回 `rejected` 并 `approval_end` 清屏
  - `mirror`（默认）下 `next()` 与设备弹窗同时开始，谁先答听谁的；超时同样返回 `rejected`
  - 设备点「转电脑」（`toPc` / `fallback`）→ `next()`
  - 配置：`deviceApproval: 'prefer'|'mirror'|'off'`，`fallbackMs`
- 监听 `agent/status`、`tool/call`、`turn/start|end` → publish 到 `nova/<id>/state`（retain）
- 内置 MQTT 客户端（`mqtt` npm 包），连 broker；断线自动重连
- 内置四个出网抓取器（DeepSeek / OpenCode / OpenDesign / ModelDial），独立失败域
- 缓存 `~/.dsh/nova-buddy/cache.json`，设备重启不受影响

**不再需要 Python 桥进程**，抓取全部搬进插件（Node 有原生 fetch，cookie 读取走同样的 Keychain 授权）。

---

## 12. 开发与验证方式

### 12.0 本机工具链（已核实，2026-11-21）

工具链**已经装好了**，但两条符号链接是坏的，直接用 `pio` / `idf.py` 会报「No such file or directory」。

#### 实际位置

| 项 | 值 |
|---|---|
| 工具链根目录 | `/Volumes/hamelin-ssd/01-项目工程/devtools`（28.6 GB） |
| PlatformIO Core | `/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio`（6.6 GB，**v6.1.19**） |
| ESP-IDF | `/Volumes/hamelin-ssd/01-项目工程/devtools/.espressif`（22 GB） |

**坏掉的符号链接**（都在 `~`，指向不存在的 `/Volumes/hamelin-ssd/devtools`）：

```
~/.platformio  ->  /Volumes/hamelin-ssd/devtools/.platformio      ✗ 目标不存在
~/.espressif   ->  /Volumes/hamelin-ssd/devtools/.espressif       ✗ 目标不存在
```

真实路径比链接目标多一层 `01-项目工程/`。修法二选一：

```bash
# 方案 1：重指符号链接（推荐，长期有效）
ln -sfn /Volumes/hamelin-ssd/01-项目工程/devtools/.platformio ~/.platformio
ln -sfn /Volumes/hamelin-ssd/01-项目工程/devtools/.espressif  ~/.espressif
```

```bash
# 方案 2：不改链接，用本仓库的包装脚本（已提交）
./tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8
```

#### `tools/pio.sh` 干了什么

```bash
CORE=/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio
export PLATFORMIO_CORE_DIR="$CORE"
export PYTHONPATH="$CORE/penv/lib/python3.11/site-packages"
exec "$CORE/python3/bin/python3" -m platformio "$@"
```

为什么不能直接 `$CORE/penv/bin/pio`：那个 venv 的 `python3` 也是符号链接，指向 `~/.platformio/python3/bin/python3`（同样坏掉）。所以必须用 `$CORE/python3/bin/python3`（**真实的自带解释器**，实测可用）+ `PYTHONPATH` 指向 venv 的 site-packages。

`PIO_CORE_DIR` 环境变量可覆盖默认路径，工具链搬家时改这一个变量即可。

#### 已装组件

| 组件 | 版本 | 位置 |
|---|---|---|
| PlatformIO Core | 6.1.19 | `/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio/penv/lib/python3.11/site-packages/platformio` |
| pioarduino 平台 | **53.03.13**（本项目用） | `/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio/platforms/espressif32` |
| PlatformIO 官方 espressif32 | 7.0.1（备用） | `/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio/platforms/espressif32@7.0.1` |
| Arduino-ESP32 framework | 3.1.3 | `/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio/packages/framework-arduinoespressif32` |
| xtensa 工具链 | esp-13.2.0 / esp-14.2.0 / esp-15.2.0 | `/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio/packages/toolchain-xtensa-esp-elf` |
| esptool | v4.8.6 | `/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio/packages/tool-esptoolpy` |
| ESP-IDF | **v5.3.5 / v5.4.4 / v5.5.4 / v6.0.1** 四套齐全（含源码树） | `/Volumes/hamelin-ssd/01-项目工程/devtools/.espressif/v{ver}/esp-idf` |
| IDF Python venv | 每版一套 | `/Volumes/hamelin-ssd/01-项目工程/devtools/.espressif/tools/python/v{ver}/venv` |
| openocd / ninja / esp-clang | 有 | `/Volumes/hamelin-ssd/01-项目工程/devtools/.espressif/tools/` |

**结论：不需要再装任何东西。** 28.6 GB 里该有的一应俱全，包括四套完整 IDF 源码 —— 如果将来要从 Arduino 侧迁到纯 IDF（`ESP32S3_AMOLED_IDF` 那个仓库，本地 `/Volumes/hamelin-ssd/embedding/ESP32S3_AMOLED_IDF`），工具链现成。

#### 构建验证结果（本机实测）

```
$ ./tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8
RAM:   [===       ]  25.7% (used 84324 bytes from 327680 bytes)
Flash: [==        ]  18.5% (used 1555264 bytes from 8388608 bytes)
SUCCESS  00:02:20
```

| 产物 | 值 |
|---|---|
| `firmware.bin` | **1,555,632 B = 1.483 MB** |
| RAM 占用 | 84,324 B（25.7%，不含 PSRAM） |
| 编译耗时（首次） | 140 s |
| 编译耗时（增量） | 9.6 s |
| 零警告 | ✅ |

#### 烧录命令（改分区表后必须先 erase）

```bash
./tools/pio.sh run -t erase        # 清 flash（分区表变了必须做）
./tools/pio.sh run -t upload       # 烧固件
./tools/pio.sh run -t uploadfs     # 烧 LittleFS 素材
./tools/pio.sh device list         # 查串口
./tools/pio.sh device monitor      # 看日志（115200）
```

#### IDF 环境（备用路线）

```bash
# 激活脚本已生成，但里面写的是坏链接路径，需要先修 ~/.espressif 符号链接
source /Volumes/hamelin-ssd/01-项目工程/devtools/.espressif/tools/activate_idf_v5.5.4.sh
idf.py set-target esp32s3
idf.py build
```

注意：`/Volumes/hamelin-ssd/01-项目工程/devtools/.espressif/tools/activate_idf_v5.5.4.sh` 里硬编码了 `IDF_TOOLS_PATH:/Users/hamelin/.espressif/tools` 等路径（见文件头 `get_env_var_pairs`），**只有修好符号链接后才能用**。不改链接就得手动 export 那六个变量。

### 12.1 画面获取（不靠反复烧录）

UI 实际用到的 canvas API 只有 17 个：`setTextColor / setCursor / setTextSize / setFont / print / printf / fillScreen / fillRect / fillRoundRect / drawRect / drawRoundRect / drawCircle / fillCircle / drawLine / drawFastHLine / drawPixel / draw16bitRGBBitmap`。

| 档 | 做法 | 烧录 | 延迟 | 验证什么 |
|---|---|---|---|---|
| A 主机模拟器（主用） | 抽 `Canvas` 接口，Mac 上 clang++ 编译出 PNG | 零 | <1s | 布局、字号、换行、颜色、动效时间轴 |
| B USB 截图 | `{"cmd":"shot"}` → RGB565 经 USB CDC | 加一次 | ~0.3s | 真实驱动、色偏、撕裂 |
| C MQTT 截图 | 同 B 走 `nova/<id>/up` | 无 | ~1s | 断链观感 |

A 档不需要 PlatformIO：`clang++` + stb_image_write 单头文件即可。`u8g2_font_chill7_h_cjk` 是纯 C 数组，主机直接链。

B 档体积：368×448×2 = 329,728 B（约 322 KB），USB CDC 几百 KB/s → ~0.5s；RLE 可压到 ~30–60 KB。

### 12.2 迭代节奏

```
改 UI → 主机模拟器出 PNG（1s） → 定稿
              ↓ 仅定稿或改驱动时
        ./tools/pio.sh run -t upload → USB 截图对照
```

### 12.3 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0** ✅ | ~~装工具链~~（已有）；**分区扩容**（已改 `no_ota_8mb.csv`）；`hwDisplayPushRect`；主机模拟器 + USB 截图工具 | 实测局部刷新 60.0 fps；PNG 与屏幕一致 |
| **P0.5** ✅ | **素材转换链路**（`tools/assets/convert_pet.py`，9 个 `.nva` 已出，3791.9 KB） | LittleFS 烧录通过；真机角色已渲染 |
| **P1** ✅ | WiFiManager 配网 + MQTT 连通（`192.168.31.15:1883`）+ 协议 + 状态模型 + 主屏/详情/设置导航 | 真机连网在线；DSH 会话数/运行数同步 |
| **P2** ✅ | 额度三卡（DeepSeek / OpenCode Go / OpenDesign） | 实测抓取 DeepSeek `CNY 77.08`、OpenCode Go `w 73% / m 66%` |
| **P3** ✅ | 排行页（ModelDial Top 8） | 实测抓取 8 行 + `CC BY 4.0` 署名 |
| **P4** ✅ | 审批闭环（固件弹窗 + `host-plugin` `approval/request` 监听与超时自动拒绝） | 端到端协议与单测通过（待你在真机触屏点按验收） |
| **P5** ✅ | `.nva` 帧序列解码（ROM `tinfl`）+ `task_decode` 双缓冲 + 动效 + `hwTone` 音效 + 自适应电源 | 诺瓦动画已真机上屏，支持触屏手势与设置切档 |

---

## 13. 未决事项（v0.3 已全部拍板）

| # | 问题 | 结论 |
|---|---|---|
| Q1 | 屏幕范围 / 默认主页 | ✅ **已定**：设置页可配默认主页（宠物 / 订阅 / 榜单），`宠物 ↔ 订阅 ↔ 榜单 ↔ 详情` 触摸左右循环滑动 |
| Q2 | 60fps 还是长续航 | ✅ **已定**：自适应双策略（插电 60fps 不熄屏；电池 30fps + 30s 熄屏 + `WIFI_PS_MAX_MODEM`，设置页可切 15fps/60fps） |
| Q3 | 是否拆 `src/main.cpp` 做 canvas 抽象 | ✅ **已定**：彻底拆分为 RTOS 多任务 + 抽象 `Canvas` 支持主机 PNG 模拟器，移除旧 BLE 与 ASCII 宠物 |
| Q4 | 角色形象 | ✅ **已定**：`dsh-pet` 蓝发小女仆（诺瓦），沿用 `128×155` `.nva` 1:1 渲染 |
| Q5 | PlatformIO 工具链 | ✅ **已解决**：`./tools/pio.sh` 现成可用 |
| Q6 | MQTT broker 放哪 | ✅ **已定**：`192.168.31.15:1883`（24 小时在线），`WiFiManager` 支持改配 |
| Q7 | broker 认证方式 | ✅ **已定**：局域网匿名访问（无密码、无 TLS） |

### 13.1 已知阻断项

1. ~~本机没有 PlatformIO~~ ✅ **已解决**：`/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio` 装了 6.1.19 + pioarduino 53.03.13，构建实测通过（见 §12.0）。
   **但 `~/.platformio` 和 `~/.espressif` 两个符号链接指向不存在的 `/Volumes/hamelin-ssd/devtools`**，直接跑 `pio` 会报找不到。修法见 §12.0。
2. **设备当前扫不到**（BLE 扫描无 NUS / `Claude-*` 广播）。需要上电并刷入本项目固件。改用 MQTT 后，这个阻断也只影响首次刷机，不影响后续联调。
3. **OpenDesign 额度需要网页会话 cookie**，不授权则该卡显示「未连接」。
4. **审批的端到端延迟未实测**。局域网 MQTT 理论 10–50ms，但从 `approval/request` 到设备弹窗还包含插件处理 + 渲染，目标 500ms 需真机验证。

---

## 14. 附：参考实现对照

| 项 | xiaozhi-esp32（origin/main） | 本项目 |
|---|---|---|
| RTOS | FreeRTOS | FreeRTOS |
| 显示 | `esp_lcd` + LVGL | Arduino_GFX + 自绘 canvas |
| 传输 | WebSocket | **MQTT**（v0.2 改，见 §3.0） |
| WiFi 省电 | `esp_wifi_set_ps` + `listen_interval=10` 三档状态机 | **同款做法**（见 §8.5） |
| 配网 | `esp-wifi-connect` 组件（AP + SmartConfig） | **同款组件** |
| 角色 | 无（纯对话设备） | `.nva` 帧序列（来自 https://github.com/PC2005-cloud/dsh-pet）+ zlib |
| 省电 | PowerSaveTimer 降亮度 + AXP 断电 | esp_pm + displayOff，不断电 |
| 深睡 | 少数板子 light-sleep + timer | 不深睡 |
| 电源键 | 独立任务 100ms 轮询 AXP | 复用现有 `hwAxpPekeyShortPress()` |
| 中断 | 同样受 TCA9554 限制 | 同 |
| 构建 | ESP-IDF（CMake） | PlatformIO + pioarduino 53.03.13 |
| 工具链 | 自带 `.espressif` 全套 | 复用本机 `.espressif`（4 套 IDF 现已装好） |

RT-Thread / Zephyr 评估结论：ESP32 侧驱动生态成本过高，且现有可跑代码全在 Arduino 侧，不迁移。
