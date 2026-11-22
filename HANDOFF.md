# Nova Buddy — 交接

仓库：`/Volumes/hamelin-ssd/gemini工作区/nova-buddy`（GitHub `JAYcodr/whale-buddy-esp32`，分支 `feature/whale-tokens`）
插件：`host-plugin/`（`dsh-plugin-nova-buddy` v0.3.0），通过 symlink 装入 `~/.dsh/profiles/desktop`
环境：DeepSeek Harness `0.2.2-rc.5`，profile `desktop`；Node v25.9.0
设备：Waveshare ESP32-S3 Touch AMOLED 1.8（SH8601 + FT3168），deviceId `D93468`，IP 192.168.31.219
Broker：`mqtt://192.168.31.15:1883`
当前 git HEAD：`6286b54`

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

---

## 测试

```bash
cd "/Volumes/hamelin-ssd/gemini工作区/nova-buddy"

# 插件单测（14 个）—— 注意 npm test 会报 Missing script
node --test host-plugin/test/bridge.test.mjs host-plugin/test/tokens.test.mjs

# state 载荷全链路（不需要真机、不需要 broker）
node tools/verify_state_payload.mjs

# 模拟器出图（29 张，含充电/满电/保养停充/低电/电源页）
bash tools/sim/run.sh

# 电量估计 + 充电策略纯逻辑单测
bash tools/test_battery.sh

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

分支 `feature/whale-tokens`，比 `main` 领先 9 个提交。要合进 main 还没做。
