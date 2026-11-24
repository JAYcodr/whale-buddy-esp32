# 第二轮评审综合：五方意见与事实修正

评审对象：[问题陈述](voice-session-bridge-brief.md)、[第一轮综合](voice-session-bridge-review-1.md)
参与：DeepSeek、Kimi、crbon、hydrogen、hy4
日期：2026-11-24

> 这一轮的价值不在"多了三种意见"，而在于**问题陈述本身被证伪了四处**。
> 下面先列事实修正（我逐条验证过），再谈判断收敛。

---

## 一、问题陈述的四处事实错误（已核实）

| # | 原文说法 | 真相 | 证据 |
|---|---|---|---|
| 1 | 「投递错：无防」 | **回执是原生的**：`prompt({requestId})` → `follow()` → 匹配 `user/message` 的 `source.rpcId` → 只有 admitted 之后的 `turn/end` 才算自己这一轮 | `~/.dsh/profiles/desktop/node_modules/@kenz1117/dsh-ui-usage-billing/lib/index.js:9956-10080` |
| 2 | 「TTS 超长 = 固件静默丢弃，不受控」 | **NVAD 在宿主侧编码**：`encodeNvAdpcm` 分配 `16 + ceil(n/2)`，偏移 8 就是采样点数 `n`。发包前把 `n` clamp 到 196500（≈12.28s @16k）并做 80ms 淡出，**零固件改动**根治 | `host-plugin/lib/adpcm.js` `encodeNvAdpcm` |
| 3 | 「投影 schema 不够，要重构」 | **字段都在**。问题是 `turn/end` 把 `cur.msg` 无条件冲成「待命中」——用户问「在干啥」恰好发生在信息被清空的一刻 | `host-plugin/lib/bridge.js:671-686` |
| 4 | 「`nova_sessions()` 用 title 概括会话」 | **title 被截成 18 字**：`foldTitle` 是 `slice(0,9)…slice(-8)` | `host-plugin/lib/bridge.js:58-69` |

第 1 条最要紧：**「缺回执」是五方里三个模型共同的判断，而它建立在错误前提上。** 推论：origins 表不用自己造，ack 协议不用在设备端另设计。

第 2 条次之：它把「TTS 静默丢弃」从"不可控的物理约束"降级为"宿主侧一行的 clamp"。

---

## 二、上一轮我就该注意到、但漏了的两个现网事实

### 2.1 屏上 60 字、嘴上 40 字（两个模型独立撞上）

- `ai.js:275` 要求模型「60 字以内」
- `cleanChatReply` 截成 59 + …
- `bridge.js:110` `TTS_MAX_CHARS = 40`，`clipForTts` 截成 39 + …

同一条回复，屏上和嘴上**是两个不同版本，且都是截前半句**——正是方案 A 的死穴，只是长度从 1000 换成了 60。

这不是假设，是**已经在线运行的现象**。第 6 节「假设 1 合规率未知」可以直接删掉：有日志就能出基线。

### 2.2 插件现在根本拿不到 `sessionController`

`host-plugin/index.js:14` 只有 `export const inject = ['tokenMeter']`，整个插件是纯 `session/event` 观察者。

要走 `prompt()` 必须 `ctx.inject(['sessionController'], activate)`——**异步激活**，而 `bridge.start()` 是同步的。MQTT 一连上就可能来 `chat_send`，那一刻 controller 还没就绪。

**这是第一步里唯一会真炸一下的地方，前两轮都没提。**

---

## 三、五方的收敛（不再讨论的部分）

| 结论 | 支持度 |
|---|---|
| 删掉插件直连 LLM 作为**对话主体** | 5/5 |
| 不做「兜底截断正文」 | 4/5（我原持不同意见，被推翻） |
| 内容相似度不做主路由信号 | 5/5 |
| 默认只读，写操作必须显式 | 5/5 |
| 不做 368px 独立列表页，改用草稿卡「内容 + 目标」双确认 | 4/5 |
| 设备端不该有自己的一套历史 | 3/5，但推理成立（两套历史必然分叉） |

**关于「删直连 LLM」的准确表述**（三方说法有歧义，需要澄清）：

不是把 `ai.js` 的通道删掉，而是**把它从「对话主体」降级为「出站压缩器」**。crbon 和 hydrogen 的方案里都还在用它——只是输入从"用户那句话"变成"这一轮的完整回复"。

---

## 四、剩下的两处真分歧

### 分歧 1：压缩发生在哪一层

| | 主张 | 优势 | 代价 |
|---|---|---|---|
| **宿主后处理**（crbon / hydrogen） | `turn/end` 后，把「用户原话 + 本轮末条 assistant + 工具成败」喂一次小模型，产出 ≤38 字 | **对工作会话零侵入**，电脑端体验不变 | 多 300–500ms；是盲摘要，可能漏掉用户真正问的点 |
| **形状契约**（hy4） | 让模型填 `{ display, speak, more }` 两个独立字段 | 生成侧约束，压缩不丢语义；`display`/`speak` 可不同长 | 只对**专属会话**成立；工作会话不能这么要求 |
| **生成侧约束**（DeepSeek / Kimi） | 请求侧就带「一句话答」，压缩在生成时发生 | 最便宜 | 对已有工作会话是污染（crbon 明确反对） |

**裁决：双层，按会话类型分。**

- **专属会话**（设备自己那个）→ 形状契约。它的 prompt 本来就归我们管，`display` / `speak` 两个字段能解决"屏上 40、嘴上 40 其实是两个需求"这个老问题。
- **工作会话**（电脑上正在跑的那些）→ 宿主后处理。零侵入是硬要求：给工作会话塞「≤40 字」的 prompt，几轮之后电脑端也会跟着变傻（crbon 这条判断成立）。

### 分歧 2：设备的身份是什么

| | 主张 | 关键论据 |
|---|---|---|
| crbon | **桌面总台**，不是小号 DS-Bot | 设备离主显示器只有 40cm。你看主屏时不需要它复述；你没看主屏时也审不了 diff |
| hydrogen | **全局调度员**，显式绑定后切单会话遥控 | 「读」成本归系统，「写」成本归人 |
| hy4 | **你漏了一类**：设备已经是**带审批权的端点** | `onApprovalRequest` 能批/拒工具，自带 `apId`，不受换段影响——这是最容易做对的一类上行 |

**裁决：hy4 的最准，但要用 crbon 的定位来排权重。**

设备此刻已经能干三件事：看进度、批工具、说话。**审批是最强的既有功能**——它有硬时限（20s）、有稳定的 `apId`、不依赖 sessionId、换段不影响。而"聊天"是三者里最弱的。

所以定位应该是：

> **审批终端 + 巡检看板（默认只读） + 按需投递（显式目标）**

聊天是第四位。这也解释了为什么"多轮气泡 8 条"这个设计一直别扭——它在为一个排第四的功能做最好的界面。

crbon 的"离显示器 40cm"是这个判断的地基：**桌面设备的稀缺资源不是屏幕，是"你正看着它"这个时刻**。那个时刻最该显示的是"要不要批"和"卡在哪"，不是聊天记录。

---

## 五、修正后的第一步

跟第一轮相比，动作少了、规格更实：

1. **`index.js`**：`ctx.inject(['sessionController'], ...)` 里才 `bridge.start()`，并给 bridge 注入 controller。
   （风险：controller 就绪前的 `chat_send` 要排队或丢弃，不能静默断链）
2. **`handleChatSend`**：删 `generateChatReply` 作为回复通道，改
   ```js
   const frames = controller.follow({ address: { kind:'session', sessionId } }, signal)
   const requestId = `${deviceId}:${clipId}:${seq}`   // 注意 uint16 clipId 会绕回，必须带单调序号
   await controller.prompt({ requestId, sessionId, mode:'queue', content:[{type:'text',text}] }, signal)
   // watch: 命中 source.rpcId === requestId → admitted；admitted 之后的 turn/end 才认领
   ```
3. **专属会话 system prompt**：单处定义预算（屏 / 嘴各自一个数），不允许列点。
4. **出站闸门**：`display` 和 `speak` 两个字段，各过同一个计量函数（按显示宽度，不按字符数）。
5. **NVAD**：发包前 `n = min(n, 196500)`，末尾 80ms 淡出。**零固件改动**。
6. **`turn/end` 不再冲掉 `msg`**，加 `lastAction / lastActionAt`；`ageSec` 现算不存。

**这一周明确不做**：`nova_say`、`nova_sessions` 工具、多会话列表页、语音分段续播、兜底截断。

先把「注入 → 认领 → 压缩 → 上屏」这条闭环跑通，用真实语料替下一周做决定。

---

## 六、我不接受的一条

**hy4 说：`turn/end` 且 `!admitted || !said` 应该成为显式可观测事件，而不是补一句像正常回复的话。**

我接受这个原则（掩盖故障比不回复更坏），但**不接受它的极端形式**："什么都不显示"。

设备的存在意义就是"人不在电脑前"。如果那一刻屏幕完全沉默，用户无法区分「网络断了」「模型没说话」「还在跑」。这三个状态必须**在视觉上可区分**：

- 收到但没说完 → 一个明确的「它没说完」状态位
- 根本没收到 → 「未送达」状态位
- 还在跑 → 计时器

**"可观测"不等于"沉默"，等于"状态可见"。** 这是我唯一保留的分歧。
