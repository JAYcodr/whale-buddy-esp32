# Nova Buddy — 交接

仓库：`/Volumes/hamelin-ssd/gemini工作区/nova-buddy`（GitHub `JAYcodr/whale-buddy-esp32`，分支 `feature/v050-aod`）
插件：`host-plugin/`（`dsh-plugin-nova-buddy` v0.3.0），通过 symlink 装入 `~/.dsh/profiles/desktop`
环境：DeepSeek Harness `0.2.2-rc.5`，profile `desktop`；Node v25.9.0
设备：Waveshare ESP32-S3 Touch AMOLED 1.8（SH8601 + FT3168），deviceId `D93468`，IP 192.168.31.219
Broker：`mqtt://192.168.31.15:1883`
固件版本：`0.5.0`（AOD + 观测）；当前 git HEAD 见 `git log -1`

---

## Issue 状态：全部关闭

| # | 标题 | 结论 |
| --- | --- | --- |
| 1 | 角色素材 CC BY-NC-SA 署名 | 已补（`8d35f2e`） |
| 2 | 主屏底部信息栏要能装下更多内容 | 真机验证通过 |
| 3 | 配网出二维码 | 手机实扫成功 |
| 4 | 额度空态写「未连接」 | 真机验证通过 |
| 5 | AP 页防烧屏 | 真机 35 秒位移验证通过 |

---

## 链路现状（全部正常，已真机实测）

- 上行：`nova/<id>/hello`（retain）、`nova/<id>/up`（30s）
- 下行：`time`（30s）、`quota`（~4min）、`rank`（6h）、`choreo`（15min）、`tokens`（60s）、`state`（事件驱动）
- 事件驱动链已通：`run` / `sess` / 工具名 / 上下文占比 / 会话标题全部回填

---

## 曾踩过的坑（都已修，别再踩回去）

### 插件侧（host-plugin）

1. **监听必须带 `{ global: true }`**
   `session/event`、`session/disposed`、`approval/request` 按 agent scope 路由，
   插件监听注册在 profile 根作用域会被 cordis 过滤掉。
2. **`ctx.tokenMeter` 访问要在派发路径外求值**
   cordis 对服务属性有 inject 门槛，回调里直接访问会抛
   `cannot get property "tokenMeter" without inject`，异常被吞成一条 warn，
   表现是「事件到了但桥接函数没执行」。需要 `export const inject = ['tokenMeter']` +
   在 apply 里求值一次存变量。
3. **改完必须完全重启 DSH**（macOS 托盘图标也要退出）
   ESM 模块缓存不因改文件失效。

### 协议侧

4. **`err` 字段必须是真 bool，只能是字面量 `true`**
   ArduinoJson 7.4.3 实测 `doc["err"] | false`：`{"err":1}`、`{"err":"true"}`、
   `{"err":{...}}` 一律求值 **0**。异常原文必须走平级 `errMsg` 字段。
5. **`state` 的 `msg` 里别出现「异常」二字**
   `proto.cpp` 有 `strstr(st.msg, "异常")` 兜底判定，正常消息带这两字会误报。

### TTS 侧

6. **单条上限 98304 字节 ≈ 12.3 秒 ≈ 43–55 汉字**
   超过 `applyTtsChunk` 直接 `free + return false`，**整条静默丢弃**，无任何提示。
   播报文本必须在送进合成前压到 40 字内（`clipForTts`）。
7. **`force: true` 会让 4 秒节流失效**，必须配去重（`ttsDedupHit`，30s 窗口），
   否则连续异常语音串成一片。

### 配网侧

8. **「重新配网」要先 `s_wm->disconnect()` 再开 portal**
   主循环里有 `if (wifiUp && s_portalActive) { stopConfigPortal(); }`，
   STA 一直连着时 `wifiUp` 恒 true，portal 起来下一轮就被自己关掉 ——
   表现是「点了没反应」。
   **错误修法**：`WiFi.disconnect(true,false)` + `WiFi.mode(WIFI_AP_STA)` → 真机 panic 重启。
9. **二维码静区要自己留 4 模块**（ISO 18004）
   组件自带的 `print_console` 只用 border=2，照抄扫不出。
   另：`max_qrcode_version` 只影响 calloc 大小，不影响实际版本；本项目载荷 27 字节 → v2。

### UI 侧

10. **浮层类 UI 必须禁掉局部重绘**
    驱动动画走 `pageHomeDrawStage`，只重画舞台那块、不经过 `pageHomeDraw`，
    浮层画完立刻被覆盖。模拟器看不出来（它只调整页绘制）。
11. **AP 页绘制与命中必须共用同一矩形函数**
    两边各算一次 burnOffset 迟早差一格，按钮点歪。
12. **测试数据别用太理想的**
    码头会话名宽度写死 108px 的 bug，被「修渲染管线」这个短标题完美绕过。
    模拟器用例已改用 `issue 核查与复盘`。

---

## 电源与充电（0.4.0 起）

- **电量显示**：`src/model/battery_model.h` 里的纯逻辑模型 —— OCV 曲线（18 点）+ 充电阶段机（PRE/CC/CV/DONE）+ 满/空锚点 + 单调限速。`hwBattery()` 现在只返回缓存快照，真实采样在 bus task 每秒一次的 `hwPowerTick()`。
- **充电策略**：默认充电上限 90%（设置页 80/90/100 可改）；电流按 温和 200/300mA、自动 300/500mA、快速 500/500mA（亮屏/熄屏）；PMU die 温度 ≥49°C→200mA、≥53°C→100mA、≥59°C 停充，回落到 <44°C 恢复。
- **参数落点**：初始化在 `src/hw/power.cpp:init`（4.2V / 300mA / 预充 50mA / 终止 50mA / VBUS 限流 500mA / 库仑计 0x18 bit3）；治理在 `hwPowerTick()`；设置键 `chg_lim`、`chg_spd`。
- **串口取证**：`{"cmd":"bat"}` 返回 `pct/mv/stage/chg/limited/usb/tmp/gauge`；`stat` 也带一行电源字段。
- **单测**：`bash tools/test_battery.sh`（998 项断言，覆盖曲线单调、限速、锚点、上限 cap、温度分档、拔插重臂）。

### 电源侧别踩回去

1. `hwBattery()` 已经从「每次调用打 I2C」改成「读缓存」，别再在里面加读取；BLE `status` 也走它。
2. 库仑计（0xA4）已开但**没校准**：只写进 state 给设置页看，不参与百分比计算。
3. TS/NTC 的 JEITA 保护**没开**（`initImpl()` 默认 disable），温度用 PMU die 温度并偏保守；不要拿它当电池温度。
4. **没设** `setLowBatShutdownThreshold()`：它按未校准的 gauge SOC 触发，可能在 3.7V 就把机器硬关掉。低电锚点交给模型（3.4V/10s → 0%）。
5. 到上限停充后**拔插才会重新武装**（`if (!usb) g.limitCut = false`），显示值会一直停在 90，不会自己掉到 86 再充。
6. `battPct` 变化触发的全屏重绘有限流（5% 桶或 15s），别把限流删了，否则充电时会一直整屏推。
7. **充电使能位跨复位保持**：`0x18 bit1` 不会因为 ESP32 重启恢复。上一轮在 90% 停充后重启，必须由 `hwPowerInit()` 显式 `enableCellbatteryCharge()`，否则永远不充。
8. **上限 100% 不能当作「在 100 停充」**：`pct >= limit` 会让 100% 上限在满电时触发停充，AXP 永远到不了 DONE，界面卡在 `LIMITED`。`chargeGovernorStep()` 里 `limitPct >= 100` 直接清掉 latch。
9. **首帧 rawPct**：CV/DONE 阶段也要算 OCV 表值（只是不用它做目标），否则开机时 `rawPct=0`，满电包从 0 开始按 1%/s 爬 —— 实测刷完机显示 10%。
10. **实测（2026-11-22，室温）**：4.17V 静置时库仑计报 100%（未校准，别信）；CV 4.212V、300mA 充电时 PMU die 31°C，没触发温度降流 → 49/53/59°C 的阈值不激进。
11. NVS 跨刷机保留：改上限要在设置页点，不要指望刷机恢复 90%。

---


## AOD 与观测（0.5.0 起）

- **进入条件**：**只看端侧输入**——触摸/按键/PEK 闲置 N 分钟（设置 1/2/5，默认 2 分钟），外加无审批、非配网、屏亮着、没翻扣。**宿主的 `run/wait` 不参与判定**：跑五六个小时的任务也照样进表盘，否则就是亮一整天 + 烧屏。模式：`关 / 仅插电（默认）/ 常开`。策略全在 `src/model/aod_policy.h`。
- **退出**：触摸 / 按键 / 审批 / AP 模式 / 拔电（仅插电模式）/ 电池 ≤10%（常开自救）。任务开始或结束**不退出**，屏幕不该为这个亮起来。
- **电池常开**：AOD 接管主屏熄屏（否则 30s 一到表盘就被熄掉）；只要不在主屏、或有审批、或电量 ≤10%，仍按 `batSleepSec` 熄屏。
- **画面**：DIN 92 大时钟（字体 `g_fontDin92`，由 `tools/fonts/gen_aafonts.py` 生成；这套字没有 `:`，冒号是两个矢量圆点）+ 16px 日期 + 150×176 定格姿势 + 一行状态。姿势跟状态走：`run>0 → WORKING`、`wait>0 → WAITING`、否则 `SLEEP`；状态行 `运行 N · 会话 M · 今日 X 亿`。没有 RTC 时显示「时间未同步 / 等待宿主校时」，不画假数字。
- **防烧屏**：`burnOffset` ±2px/30s + `aod::drift` ±3px/10min + 亮度封顶 2 档（23–07 点 1 档）。

### 实测数字（2026-11-22，USB 供电，23:2x）

| 场景 | 绘制/分钟 | UI 忙时 | 解码忙时 | 主频 | push |
|---|---|---|---|---|---|
| 插电首页 60fps | 3591 | 52–64% | 4–14% | 240MHz | 10.5ms |
| AOD（无 state 事件） | 5 | ~2% | 0% | 80MHz | 51.8ms |
| AOD（会话活动，20 条 state） | 16 | 1.9% | 0% | 80MHz | 51.8ms |

触摸唤醒：`aod=0 → brt=4, mhz=240`，即时。

### 电池实测（2026-11-23 凌晨，AOD 常开，600mAh 标称）

拔电后 01:02 起到 02:10 的稳态（AOD 常亮、80MHz、亮度 1 档）：

| 时间 | 电压 | 电量 | 绘制累计 |
|---|---|---|---|
| 01:02 | 3920mV | 69% | 18613 |
| 02:06 | 3785mV | 48% | 19100 |
| 02:10 | 3779mV | 47% | 19137 |

- 稳态斜率：**−127 mV/h、−19.7 %/h**，反推平均电流 ≈ **118 mA**（600mAh × 19.7%/h）。
- 续航投影：100% → 10% 约 **4.6 h**；按 3.4V 截止约 **5 h**。和「常开只适合插电、不适合过夜」的直觉一致，默认「仅插电」是对的。
- 稳态 AOD：**5–6 次绘制/分钟**、`cpu ui 0–4%`、`dec 0%`。
- 注意：AXP2101 没有电流 ADC、库仑计未校准，pct 是 OCV 估算，以上是斜率法不是电流法。
- 观察到的抖动：拔电后头 30 分钟出现过 3 次 AOD 掉回主屏（30fps、每次 1–2 分钟）后自行恢复；稳态 1 小时未复现，成因未定位（疑似拔电瞬间的 PEK/触摸伪事件），下次插电时用 `{"cmd":"statrate","ms":5000}` 长时间观察。

### 省电（0.5.0 补丁，2026-11-23）

只做了两件便宜且不改交互的：

1. **熄屏不解码**：`animNeeded = !screenOff && (...)`，原来熄屏了 `nvaTick` 还在 10fps 空转 zlib。
2. **电池下降 WiFi 发射功率**：插电 20dBm；电池按 RSSI 分 13/17/20dBm（−72/−76dBm 迟滞），断链风险由分档兜住。`stat` 新增 `tx`（0.25dBm 单位）。
3. 没做：10fps 动画上限、静态页 5fps、亮度分档、bus 节流——电池默认熄屏、AOD 又是静态画面，这些在实际用法里触发不了；整套只能再省 10–15%，过不了一夜。判断依据见上一节实测。
- 采集脚本在那个窗口里被 DSH 重启杀掉过一次（01:02），之后用实时 `up` 补了两个点；数据文件 `~/.dsh/nova-buddy/power-…-aod-batt.csv`。

### 观测字段与采集

- `stat` 新增：`drawFps / pushUs / idle / draws / pwr / brt / lcd / aod / cpu{ui,dec} / mhz / psram`；`hello` 新增 `pwr / aod`。
- 采样：`node tools/power_probe.mjs --id D93468 --minutes 20 --tag aod-off`（落 `~/.dsh/nova-buddy/power-*.csv` + `.md` 汇总）。充电段不算斜率，脚本会明确写出来。
- 采样前可 `{"cmd":"statrate","ms":5000}` 改密（5–300s，0=默认 30s）。

### AOD 侧别踩回去

1. **AOD 期间任何 `nvaSetState` 都要被跳过**，否则宠物会被状态机改成 WAITING/IDLE（实测第一次上真机就是站着睡的）。
2. **电池熄屏条件**：`aodCoversBattery` 必须同时满足「常开 + 主屏 + 电量 >10%」，否则要么把表盘熄掉，要么把电耗干。
3. **DIN 92 没有冒号**，`--:--` 会落到 20px fallback 字体，看起来像两个小点；没时间就整块换成文字提示。
4. **`stat` 载荷缓冲**已加到 320B（net.cpp），再加字段记得一起看。
5. **AOD 强制验收钩子**：`{"cmd":"aodforce","on":1|0}`（跳过进出判定）。自然进入现在可以直接验：设 `aodidle 30` 后静置 40 秒即可，宿主任务在跑也会进。
6. **别把宿主 `run/wait` 塞回闲置判定**：那是"有没有任务"，不是"有没有人"。5 小时的长任务会让屏亮一整天，这正是 AOD 要解决的。

## 测试

```bash
cd "/Volumes/hamelin-ssd/gemini工作区/nova-buddy"

# 一条命令跑完全部（约 15s）
bash tools/check_all.sh

# 插件单测（14 个）—— 注意 npm test 会报 Missing script
node --test host-plugin/test/bridge.test.mjs host-plugin/test/tokens.test.mjs

# state 载荷全链路（不需要真机、不需要 broker）
node tools/verify_state_payload.mjs

# 模拟器出图（29 张，含充电/满电/保养停充/低电/电源页）
bash tools/sim/run.sh

# 电量估计 + 充电策略纯逻辑单测
bash tools/test_battery.sh

# AOD 策略纯逻辑单测（500 项断言）
bash tools/test_aod.sh

# 固件编译
bash tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8

# 烧录
bash tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8 -t upload
```

---

## 真机调试手法

```bash
# 截图（USB CDC，Python venv 里装了 pyserial + PIL）
.venv-shot/bin/python tools/usb_shot.py --port /dev/cu.usbmodem21101 --out /tmp/x.png

# 串口命令
{"cmd":"stat"}                    # fps / pushUs / heap / psram / mhz / bat
{"cmd":"bat"}                     # pct / mv / stage / chg / limited / usb / tmp / gauge
{"cmd":"shot"}                    # 截图
{"cmd":"reprov"}                  # 进配网模式（会断网，MQTT 掉线）
{"cmd":"page","page":"quota"}     # 切页：home/quota/rank/detail/settings/settings_adv/settings_power/tokens
{"cmd":"tap","x":184,"y":406}     # 注入点击，走 nav.onTap + handleAction 与真实触屏同路径
{"cmd":"demo","anim":3}           # 强制动画状态
{"cmd":"aod","on":0|1|2}          # AOD：关/仅插电/常开（写 NVS）
{"cmd":"aodidle","sec":30}        # 进入阈值（30..600s）
{"cmd":"aodforce","on":1}         # 验收用：不管宿主 run 计数直接进 AOD
{"cmd":"statrate","ms":5000}      # stat 上行改密（0=默认 30s）
{"cmd":"pwr","profile":1}         # 切功耗档
```

⚠️ **`animPage` 生效时设备走局部重绘**，此时 MQTT 推的 state 会被宠物动画覆盖部分区域。
手动验 UI 时注意：屏幕上的实时状态是插件推的，会覆盖你手动推的测试 payload。

**验 AP 页**：MQTT 推不了 `wifiApMode`（固件内部状态），要用串口 `reprov`。

---

## 防烧屏

每 30 秒 `burnInStep` 翻一格，偏移范围 ±2px。所有页面都必须跟 `burnOffsetX/Y`，
包括 AP 页和浮层。验证方法：隔 35 秒抓两帧做像素 diff，卡片/按钮都应该有位移。

---

## Git

分支 `feature/v050-aod`，包含 0.5.0 的 AOD + 观测 + 宿主文案清洗。验证通过后 ff 合进 `main`。
