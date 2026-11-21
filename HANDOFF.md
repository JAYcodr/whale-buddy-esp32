# Nova Buddy — 待修问题交接

仓库：`/Volumes/hamelin-ssd/gemini工作区/nova-buddy`（GitHub `JAYcodr/whale-buddy-esp32`）
插件：`host-plugin/`（`dsh-plugin-nova-buddy` v0.3.0），已通过 symlink 装入 `~/.dsh/profiles/desktop`，`plugin_manager` 显示 `fiberPhase=active`
环境：DeepSeek Harness `0.2.2-rc.5`，profile `desktop`；Node v25.9.0
设备：Waveshare ESP32-S3 Touch AMOLED 1.8（SH8601 + FT3168），deviceId `D93468`，IP 192.168.31.219，固件 0.3.1
Broker：`mqtt://192.168.31.15:1883`
当前 git HEAD：`0e91267`

---

## 链路现状（已确认正常）

MQTT 上下行全部通畅，已实测：

- 上行 `nova/D93468/hello`（retain）、`nova/D93468/up`（30s 一次 `{"evt":"stat",...}`）
- 下行 `time`（30s 心跳）、`quota`（~4min）、`rank`（6h）、`choreo`（15min）全部按周期到达设备
- 设备状态栏显示「在线」，额度页正常渲染（DeepSeek 77.08 CNY / OpenCode Go 74%·67% / OpenDesign 4%·56%）

**坏的只有"事件驱动"这条链路。**

---

## 问题 1：插件收不到任何会话事件

### 症状

- 设备永远显示 `运行 0 · 会话 0`（`state` 报文的 `run`/`sess` 恒为 0）
- 审批弹窗从不出现
- 上下文占比永不显示

### 已修的根因（提交 `0e91267`）

`session/event`、`session/disposed`、`approval/request` 都按 agent scope 路由。派发时 cordis 过滤：

```js
// @deepseek-ai/cordis/src/events.ts  _resolve()
.filter(hook => hook.global || !filter || filter.call(thisArg, hook.ctx))
```

`scopeTarget` 的过滤器（`packages/core/scope/src/index.ts:167-183`）只放行「无 scope 标签的监听」或「标签命中 agent scope 链的监听」。插件监听注册在 profile 根作用域 → 被过滤掉。

**修法**：三个监听全部加 `{ global: true }`，审批额外 `prepend: true`。参照 `xmanrui/dsh-im`（12 处注册全带 `global: true`）。

修完后事件**确实进来了**（探针日志里能看到大量 `step/start`、`turn/start`、`tool/call`），但桥接函数仍未执行。

### 未修的第二层根因

监听器回调体第一行是：

```js
ctx.on('session/event', (session, event) =>
  bridge.onSessionEvent(session, event, ctx.tokenMeter), { global: true })
```

`ctx.tokenMeter` 这个属性访问**在回调执行时**才求值。cordis 对服务属性有 inject 门槛（`@deepseek-ai/cordis/src/reflect.ts:152`）：

```
Error: cannot get property "tokenMeter" without inject
```

该异常抛出后被 `invokeContainedSessionObservers`（`packages/core/session/src/index.ts:404-424`）接住，只打一条 `ctx.logger.warn`，**不中断派发**。结果：事件到了、探测代码打了日志、`bridge.onSessionEvent` 从没被调用。

### 修法（2026-11-21 23:31 真机验证通过）

`host-plugin/index.js`：

```js
export const inject = ['tokenMeter'];
```

并把服务访问移出派发路径，求值一次：

```js
let tokenMeter;
try { tokenMeter = ctx.tokenMeter; } catch { tokenMeter = undefined; }
// ...
ctx.on('session/event', (session, event) => bridge.onSessionEvent(session, event, tokenMeter), { global: true })
```

依据：`tokenMeter` 由 `dsh-base/cordis.patch.yml:352-353` 的 `token-meter` 条目提供，始终存在。`readCtxPct`（`lib/bridge.js:28-36`）已有 `typeof ... !== 'function'` 兜底。

### 验证方法

1. 完全重启 DSH（macOS 托盘图标也要退出，ESM 模块缓存不会因改文件失效）
2. 跑一个任务，同时订阅 `nova/D93468/state`：

```bash
node -e "
const mqtt=require('$PWD/host-plugin/node_modules/mqtt');
const c=mqtt.connect('mqtt://192.168.31.15:1883');
c.on('connect',()=>c.subscribe('nova/D93468/state'));
c.on('message',(t,p)=>console.log(p.toString()));
"
```

预期看到 `{"t":"state","run":1,...}` 和 `msg` 里出现工具名。若仍为 `run:0`，说明还有第三层。

---

## 问题 2：审批时序不对

### 症状（用户原话）

> 权限弹窗是可以收到的 但是确认时间太短 并且是 pet 先出 时间到期才 客户端出现

### 根因

`host-plugin/index.js:22-23` 的两个默认值组合：

```js
deviceApproval: 'prefer',   // 设备独占决定权
fallbackMs: 8000,           // 8 秒
```

`prefer` 模式下 `onApprovalRequest`（`lib/bridge.js:449-496`）死等设备 8 秒才 `next()` 交给客户端：

```js
const res = await waitDevice;
if (res.kind === 'decided') return res.outcome;
if (res.kind === 'abort') return 'rejected';
return next();          // ← 8 秒后才轮到客户端弹窗
```

设备侧倒计时直接读这个值：`src/net/proto.cpp:165` 的 `st.apTtlMs = doc["ttl"] | 8000`，`src/ui/page_approval.cpp:16-18` 拿它当进度条总长。

### 修法（设备侧已验证：`approval` 报文 `ttl=20000` 实到）

改成 `mirror`（两边同时出，谁先答算谁的）+ 拉长倒计时到 20 秒：

```js
deviceApproval: config.deviceApproval || 'mirror',
fallbackMs: config.fallbackMs ?? 20000,
```

`mirror` 分支在倒计时归零时不会丢请求，会继续 `return local.then((r) => r.outcome)` 等客户端答案（`lib/bridge.js:488-489`）。

### 待确认

- 20 秒是否合适（要不要更长）
- 是否需要设备端在客户端已答后立即自关弹窗（目前靠 `approval_end` 报文，`settleApproval(..., {notifyDeviceEnd:true})` 会发）

---

## 测试

```bash
cd "/Volumes/hamelin-ssd/gemini工作区/nova-buddy"
timeout 40 node --test host-plugin/test/bridge.test.mjs    # 7/7 通过
```

注意：`npm test` 会报 Missing script，必须直接 `node --test`。

---

## 验证记录（2026-11-21 23:31–23:35，真机）

**问题 1 已修复。** 订阅 `nova/D93468/state`，本会话的工具调用直接产生：

```json
{"t":"state","run":1,"wait":0,"sess":1,"msg":"整理 bash 结果...","ctx":{"pct":4},"model":"deepseek-flash"}
```

随后 `ctx.pct` 从 4 涨到 5，`msg` 跟着工具从 "整理 bash 结果..." 切到 "整理 job_output 结果..." ——
`run` / `sess` / 工具名 / 上下文占比全部回填，事件驱动链路整条通了。

**问题 2 设备侧已验证。** 触发两次真实审批（越界 `touch` 探针文件，随后删除），
`nova/D93468/approval` 收到：

```json
{"t":"approval","tool":"bash","ttl":20000,...}
```

`ttl` 已是 20000，`src/net/proto.cpp:165` 把它写进 `st.apTtlMs`，进度条总长随之变长。

审批时序两点都已目视确认（2026-11-21 23:50）：

- **两端同时出现** —— `mirror` 分支在发报文的同一轮就调了 `next()`，客户端 GUI 卡片
  与设备弹窗同步弹出，「pet 先出、到期才轮到客户端」的旧症状消失
- **在 GUI 上点批准后设备立刻关窗** —— `approval_end` 链路正常。此前两次测试没抓到
  `approval_end`，是因为两次都在设备上点的：`handleUp` 的 `decision` 分支按设计
  `notifyDeviceEnd: false`，设备自己知道结果，无需回灌

`node --test host-plugin/test/bridge.test.mjs` 7/7 通过。

---

## 如果还要往下查

优先级从高到低：

1. ~~验证 `inject` 修复~~ —— 已完成，`run=1` 实测通过
2. 若仍不行，用探针法定位：在监听器体首行和 `bridge.onSessionEvent` 内部各插一行 `appendFileSync('/tmp/x.log', ...)`，看断在哪一层
3. 注意 `plugin_manager` 禁用/启用**会**重新 `apply()`（已实测，`apply ran` 每次递增），但 ESM 缓存对 `lib/bridge.js` 的改动不一定生效 —— 改 bridge 后务必整体重启
4. cordis 的 `isSpecialProperty`（`reflect.ts:86-91`）会放过 `_` 开头与符号属性，不受 inject 门槛约束

## 参考实现

`xmanrui/dsh-im`（本地 `~/.dsh/profiles/desktop/node_modules/@xmanrui/dsh-im`）是同场景的正确做法：
- `plugin-src/host/index.mjs:35-39` — `export const inject = ['connection','credentials','typertGateway']`
- `plugin-src/host/modern-harness-api.mjs:296` — `ctx.on('approval/request', ..., { global: true, prepend: true })`
- `plugin-src/host/session-sync-coordinator.mjs:392` — `ctx.on('session/event', ..., { global: true })`
