# 实施交接：设备语音端点接入 DSH 会话

> 给执行模型的自包含规格。读这一份就够，不需要回溯任何讨论记录。
> 视觉基准：`docs/design/voice-bridge-final.html`（可交互）、`voice-bridge-final-states.png`（六态截图）。
> 规格里凡是标「已定」的都是拍过板的，不要重新设计；标「待验证」的是假设，实现时要给出数据。

---

## 一、一句话目标

设备上现在有一套「插件自己调 LLM」的聊天（`bridge.js` 的 `handleChatSend` → `generateChatReply`，独立 12 条历史）。
把它**删掉**，改成：设备说的话注入一个 DSH 专属会话，会话的回复经宿主压缩后回到设备屏幕和喇叭。

设备不再有自己的大脑，它只是 DSH 的一个端点。

---

## 二、硬件与现有链路（事实，可核对）

| 项 | 值 | 出处 |
|---|---|---|
| 设备 | Waveshare ESP32-S3 Touch AMOLED 1.8，368×448，触摸 + 麦 + 喇叭 | — |
| 网络 | MQTT `mqtt://192.168.31.15:1883`，deviceId `D93468` | `HANDOFF.md` |
| 上行 | `nova/<id>/up`、`nova/<id>/hello`、`nova/<id>/mic/{start,chunk,end}` | `src/net/net.cpp` |
| 下行 | `state / quota / rank / choreo / tts / chat / time` | 同上 |
| 录音格式 | 16kHz/16bit 单声道 PCM，6 字节小头 `[clipId:u16, seq:u16, total:u16]`，每包 2048B | `host-plugin/lib/asr.js:13` |
| ASR | 小米 MiMo `mimo-v2.5-asr`，15s 超时，空文本判失败 | `host-plugin/lib/asr.js:285` |
| 插件 | `host-plugin/`，约 3000 行 JS，`dsh-plugin-nova-buddy` | `host-plugin/index.js` |

**设备端 UI 现状**：`src/ui/page_chat.cpp`（729 行），多轮气泡、最多 8 条（`state.h` 的 `MAX_CHAT_MSGS`）、1.5s 倒计时草稿卡。**这套要重做**，见第六节。

---

## 三、已定稿的交互（不要再设计）

### 3.1 主界面 = 巡检看板

```
┌ 状态栏（时间 / 电量） ──────────────┐
│ 诺瓦                                 │
│ 全局 · 只读巡检          [1 项待批]  │
├──────────────────────────────────────┤
│ ● 电量估算四改            跑着 4:12  │  ← 会话卡 ×3
│   cargo test                         │
│   nova-buddy · 2 个断言失败          │
├──────────────────────────────────────┤
│ ◐ ASR 会话桥接              等审批   │  ← 琥珀左边条
│   bash · rm -rf ./recordings         │
├──────────────────────────────────────┤
│ ○ DS-Bot 调研                停着    │
├──────────────────────────────────────┤
│ 上次播报 · 4 分钟前 · 点按重播        │
│ 单测还剩 2 个失败，都在 fixtures…    │
├──────────────────────────────────────┤
│           [ 按住说话 ]               │
└──────────────────────────────────────┘
```

- **没有历史、没有滚动列表**。8 条气泡那个设计删掉。
- 会话卡三行：名称+状态 / 当前动作 / 归因（目录 · 影响）。
- 底部「上次播报」= 你不在电脑前时错过的最后一句话。

### 3.2 点卡片 = 展开详情 + 设为目标

**必须有强选中反馈**，三处同时变：

1. 整卡高亮（背景 `#1a2a44`）+ 左侧 4px 青色竖条
2. 右上角实心圆 + 勾（`✓`），带 180ms 弹出
3. 顶部 header 变成「发往 · 电量估算四改」

其余卡片自动压成**单行**（只留名字 + 状态点 + 时长）。

原因：原设计的角落 9.5px 小标签「发往这里」需要读字才懂，等于没有反馈。

### 3.3 按住说话 → 草稿卡

```
按住底部条（≥0.35s）→ 松开
  ↓
┌ 识别到 ────────────────────────────┐
│ 接着弄电量那个，把测试跑完         │
│                                     │
│ 发往  电量估算四改      换一个 ›    │
│                                     │
│ [ 取消重说 ]        [ 发送 ]        │
└─────────────────────────────────────┘
```

**已定，两条硬规则：**

1. **没有倒计时，永不自动发送。** 2.5 秒读不完一行字，自动发等于不给人看的时间。说错了就取消重按，语音成本比确认成本低。
2. **目标确认保留**（「发往 xxx」+「换一个」），因为标题你记不住，但看一眼就能确认锁没锁对。

**不做**：可编辑输入框（那是手机，不是这块屏）。

### 3.4 发送后 = 单卡回执（状态 + 正文合并）

**已定：一张卡，不是两张。** 分开会让人以为是两个独立事件。

```
✓ 已送达 · 4 秒前
单测还剩 2 个失败，都在 fixtures 的时区断言上。
▶ 播报
```

边框颜色跟状态走：

| 状态 | 颜色 | 顶部 | 正文区 |
|---|---|---|---|
| 已送达 | 青 `#48c6ff` | `✓ 已送达` | 排队位次 / 回复正文 |
| 在跑 | 琥珀 `#ffc44c` | `● 在跑 · N 秒`（秒数跳动） | “它正在处理这句话” |
| 未送达 | 红 `#ff586c` | `✗ 未送达` | 原因 + “这是唯一会真丢东西的” |
| 没说完 | 灰 `#8294b4` | `◌ 它没说完` | “不是网络问题” |

**三种「没回音」必须在视觉上分开**。这是跟评审最大的分歧点：他们说没说完就显式报错别补一句像回复的话，但完全沉默更糟——人在两米外分不清断网、没说话、还是卡住了。

### 3.5 审批 = 最高优先级

设备**已经在做**审批（`bridge.js:736` `onApprovalRequest`），这是它最强的既有能力：自带 `apId`、20s 硬时限、不受 session 换段影响。

```
┌ 需要你批准 · 工具调用 ────── 20s ─┐
│ bash rm -rf ./recordings           │
│ ASR 会话桥接 · 会删除 6 个测试录音  │
│ ▓▓▓▓▓▓▓▓░░░░░░░░                   │
│ [ 拒绝 ]            [ 允许 ]       │
└────────────────────────────────────┘
```

倒计时归零 → 自动拒绝（系统行为，不是设计选择）。

---

## 四、DSH 侧怎么做（已确认的事实 + 代码）

### 4.1 注入用户消息并认领这一轮

```js
// index.js —— 注意：必须是异步激活
export const inject = ['tokenMeter'];
export function apply(ctx, config) {
  // sessionController 异步就绪，不能同步取
  ctx.inject(['sessionController'], (sctx) => {
    bridge.start(sctx.sessionController);   // 就绪后才 start
  });
  // 服务未就绪前到达的 chat_send 必须排队，不能静默丢
}
```

```js
// bridge.js
async function injectToSession(deviceId, clipId, text) {
  const sessionId = resolveTargetSession();           // 见 4.3
  if (!sessionId) return { ok:false, err:'未绑定会话' };

  const frames  = controller.follow({ address: { kind:'session', sessionId } }, signal);
  const requestId = `${deviceId}:${clipId}:${monoSeq++}`;   // clipId 是 uint16，会绕回
  await controller.prompt({
    requestId, sessionId, mode: 'queue',        // queue 不打断；要打断才用 steer
    content: [{ type:'text', text }],
  }, signal);

  // 认领：命中 source.rpcId 之后，这一轮的 turn/end 才算自己的
  let admitted = false;
  for await (const f of frames) {
    if (f.type !== 'event') continue;
    if (f.event.type === 'user/message') {
      if (f.event.data.source?.rpcId === requestId) admitted = true;
      continue;
    }
    if (!admitted || f.event.type !== 'turn/end') continue;
    // 这一轮结束了 → 走压缩 → 回推设备
  }
}
```

**这段不是我们发明的**。本机 `~/.dsh/profiles/desktop/node_modules/@kenz1117/dsh-ui-usage-billing/lib/index.js:9956-10080` 是可直接抄的完整范式（`follow` → `prompt({requestId})` → `watch` 里比对 `data.source.rpcId`）。先读它再写。

### 4.2 三条别踩

1. **`UserMessage.source` 不进 prompt**。provider 只取 `role + content`。想让模型知道“这条来自设备”，必须写进 `content` 或走 `agent.inject()`。`source` 是给宿主/审计用的，模型看不见。
2. **`sessionId` 不稳定**。上下文压缩会开新段。绑死 sessionId 迟早投进已归档的段。要绑逻辑会话 key，注入前解析到当前段。
3. **不能在 `session/event` 监听器里调 `session.append()`**（重入守卫）。要改挂 `agent/inbox/inserted`。

### 4.3 会话归属（这一块还没定死，实现时按优先级做）

宿主**拿不到 GUI 焦点**——`activeSession / focusedSession` 在 `dsh-api-session-controller` 和 `dsh-client-ui-session` 里都不存在。所以：

| 优先级 | 信号 | 说明 |
|---|---|---|
| P0 | 设备端显式选中 | 点卡片设的 target |
| P1 | 屏幕指针 | 插件记「最后一次推给设备的内容来自哪个会话」，单独变量，**别复用 `buildStatePayload` 的 `chosen`**（它按 waiting>running>error 选“最该报警的”，会把“接着弄那个”指向正在等审批的会话） |
| P2 | 专属会话兜底 | 永远成功、永不串台 |
| P3 | 标题语义 | **只用于重排候选或反问，永不自动投递** |

**硬规则**：自动路由**永不给 `running` / `waiting` 的会话发 steer**。往等审批的会话里 steer 可能触发意外批准。

裁法：不确定就问，不回退猜测。投错是已发生的事故，问一句只是一轮语音。

---

## 五、出站：40 字从哪来（这一节关系到会不会白干）

### 5.1 40 字不是物理上限

- 固件 `MAX_TTS_BYTES = 98304`（`src/net/proto.cpp:268`）是**单条消息的缓冲上限**，不是“不能播更长”。
- 98304 × 8 / 64000 = 12.288 秒（64kbps ADPCM）。
- 音频队列深 16（`src/hw/audio.cpp:438`），**支持排队**。
- 但每次入队 `++s_voiceGen`，播放前检查 `r.voiceGen == s_voiceGen` → **新语音取消队列里所有旧的**。所以连发 3 段只会播最后一段。

**改成「同批共用 voiceGen」，长回复就能分段播。约 10–20 行。**

### 5.2 零固件改动的根治

`host-plugin/lib/adpcm.js` 的 `encodeNvAdpcm` 在**宿主侧**编码：`Buffer.allocUnsafe(16 + ceil(n/2))`，偏移 8 是采样点数 `n`。

**发包前 `n = Math.min(n, 196500)` + 末尾 80ms 淡出**，`~196500/16000 ≈ 12.28s`，永远不会超 96KB。

这一步做完，“TTS 超长 → 固件静默丢弃”从根上消失，不需要改固件。

### 5.3 出站闸门的要求

1. **所有出站文本走同一个计量函数**，不只是 `nova_say`。错误提示、兜底文本、确定性摘要都要走——错误文本最长，最容易被整条丢掉。
2. **计量单位是显示宽度，不是字符数**。现在 `ai.js:275` 要求 60 字、`bridge.js:110` 的 `TTS_MAX_CHARS` 是 40，同一条回复屏上 60、嘴上 40，**各截一半**。这个割裂要修。
3. 屏显和 TTS 建议拆成 `display` / `speak` 两个字段，各自过计量。

### 5.4 压缩放哪

- **专属会话**（设备自己那个）→ 形状契约：prompt 里约束输出形状，不走后处理。
- **工作会话**（电脑上跑的那些）→ **宿主后处理压缩，零侵入**。绝对不要给工作会话塞「≤40 字 / 不用 Markdown」的 prompt——几轮之后电脑端也会跟着变傻。

---

## 六、设备端 UI 重做规格

视觉基准以 demo 为准，以下是要落下去的量：

| 元素 | 规格 |
|---|---|
| 屏幕 | 368×448，纯黑底 `#000` |
| 卡片 | 背景 `#0c121e`，边框 `#1e304e`，圆角 14 |
| 选中卡 | 背景 `#1a2a44`，边框 `#48c6ff`，左侧 4px 青色条，右上角 17px 实心勾 |
| 等待审批卡 | 边框 `rgba(255,196,76,.55)`，左侧 3px 琥珀条 |
| 字号 | 卡片标题 15px 600 / 动作 12px / 归因 10.5px；气泡 14.5px；状态栏 11px |
| 状态点 | 8px 圆：跑 `#48c6ff` 呼吸 / 等审批 `#ffc44c` 快闪 / 停 `#4a5c7c` |
| 底部 dock | 高 74，按住条 44px 高、圆角 22、青边框 |
| 录音层 | 全屏覆盖 `rgba(0,0,0,.965)`，波形 15 根、计时 27px |
| 触感 | 卡片按下 0.98 缩放；勾 180ms 弹出；回执卡 200ms 上浮 |

**交互约束：**

- 录音 < 0.35s 视为误触，不产生草稿
- 录音 > 10s 自动结束并进草稿
- 上滑 >55px 进入取消态（波形与提示变红），松手放弃
- 流程图里的所有按钮都可点，没有不可点的假按钮

---

## 七、分阶段实施

### 阶段 0：插件侧最小闭环（**先做这个，一个周末**）

1. `index.js`：`ctx.inject(['sessionController'], ...)` 内启动 bridge，就绪前的消息排队
2. `bridge.js`：删 `generateChatReply` 调用，改 `follow + prompt({requestId}) + rpcId 认领`
3. `requestId` 用 `deviceId:clipId:monoSeq`（**不能只用 clipId，uint16 会绕回，QoS1 还会重复投递**）
4. 建专属会话（`ctx.agents.create()` 或复用），system prompt 约束输出形状
5. 订阅该会话的 `assistant/message` / `turn/end`，把文本推 `chat` topic

**验收**：设备的 `chat_send` 真的进了 DSH 会话，会话日志里能看到带 rpcId 的 user/message；设备上收到回复。

**明确不做**：`nova_say` / `nova_sessions` 工具、多会话路由、UI 重做。

### 阶段 1：出站闸门（第二个周末）

1. `adpcm.js` 加 clamp（见 5.2）
2. 屏显 / TTS 长度统一，修 60/40 割裂
3. 吞掉所有静默失败：固件丢弃 TTS 时回一个 `tts_drop` 事件（可选，阶段 1 做完其实不会再触发）

**验收**：发一条包含英文、数字、路径的长回复，不静默丢失；屏显与播报内容一致。

### 阶段 2：设备端 UI 按第六节重做

1. `page_chat.cpp` 拆成看板 + 草稿 + 回执三个视图
2. 卡片选中态、压行、审批卡
3. 删掉 8 条气泡列表和 1.5s 倒计时

**验收**：对照 demo 逐屏比对；真机截图；触摸热区与视觉一致。

### 阶段 3：路由

1. 屏幕指针变量（独立于 `chosen`）
2. P1-P3 优先级 + running/waiting 禁注入
3. 换段检测：解析失败就<u>报错不重试</u>，显示“会话已换段，请重选”

---

## 八、坑清单（都是真踩过的）

1. **`pointerdown` 里重建 DOM → `pointerup` 永远收不到。** 浏览器把指针 capture 在被移除的节点上。demo 第一版就死在这里，录音计时涨到 8.8 秒停不下来。修法：录音层常驻，只切 class；事件委托绑一次。
2. **`ctx.tokenMeter` 不 inject 会抛异常，且被吞成一条 warn。** 表现为“事件到了但回调第一行就炸”。`sessionController` 同理。
3. **`err` 字段必须是真 bool 字面量 `true`。** `{"err":1}` / `{"err":"true"}` 在 ArduinoJson 里一律求值 0。异常原文走平级 `errMsg`。
4. **`state` 的 `msg` 里不能出现「异常」二字**，`proto.cpp` 有 `strstr` 兜底判定会误报。
5. **浮层类 UI 必须禁掉局部重绘**，否则画完立刻被覆盖。
6. **`foldTitle` 把标题截成 18 字**（`bridge.js:58`，`slice(0,9)…slice(-8)`）。拿它做会话辨识不可靠，要显示 `cwd` 末段。
7. **`turn/end` 现在会把 `cur.msg` 冲成「待命中」**（`bridge.js:671`）。用户问“在干啥”恰好发生在信息被清空的一刻。加 `lastAction` / `lastActionAt`，别覆盖 `msg`。

---

## 九、验收总纲

一条能跑通的完整链路：

> 对着设备说「接着弄电量那个，把测试跑完」→ 屏上出识别文字 + 「发往 电量估算四改」→ 点发送 → 单卡显示「已送达 / 在跑 3 秒」→ 4 秒后同一张卡变成正文 + 播报按钮 → 电脑上的 DSH 会话里能看到这条用户消息和它的回复。

**这条链路跑通之前，其他都是次要的。**
