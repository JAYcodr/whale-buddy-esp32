# dsh-plugin-nova-buddy

DSH Host 插件，负责桥接 DeepSeek Harness 会话状态、工具审批、订阅额度与 ModelDial 榜单到桌面副屏（MQTT）。

## 安装与联调

```bash
cd host-plugin
npm install
node bin/nova-buddy.js test-push --id D93468
```

## OpenDesign 会话授权

```bash
node bin/nova-buddy.js auth opendesign --paste
```

## Token 累计页（`nova/<id>/tokens`）

设备第 2 页（TOKENS）的数据由 `lib/tokens.js` 聚合，**不经过 DSH 服务**：

- `ctx.sessions` 只列当前进程内的 live session，拿不到历史；而累计页要的恰恰是全部历史。
- 所以直接读会话日志 `$DSH_HOME/sessions/*/*/session*.jsonl.zstd`，按 magic 切 zstd 多帧（Node 内置解压器只解第一帧就报 `Unknown frame descriptor`），逐帧解析 `assistant/message` 的 usage。
- 计数口径与 `@deepseek-ai/dsh-token-meter` 的 `tokenUsage` 投影一致：同一 `(turn, step)` 的后到样本**替换**而不是累加（重试、流式 settle）。
- 按事件自身的 `time` 归日，跨天会话两天各自算对。
- 缓存 `$DSH_HOME/nova-buddy/tokens-cache.json`：首次全量约 4–5 秒（1000+ 会话），之后按 `mtimeMs + size` 增量，一轮约 100ms。
- 刷新周期 60s；内容不变不重发（否则设备端 `tokenGen` 每次自增会白重绘一页）。

payload：

```json
{
  "t": "tokens",
  "total": 4049995954,
  "spanDays": 21,
  "cachePct": 97.2,
  "days": [["11/09", 168638441], ["11/10", 107184642]]
}
```

| 字段 | 含义 |
|---|---|
| `total` | 全部历史累计（四桶之和：未命中输入 + 输出 + 缓存读 + 缓存写） |
| `spanDays` | 有数据的自然日数 |
| `cachePct` | 缓存命中率，**只看输入侧**：`cacheRead / (cacheRead + uncachedInput + cacheWrite)`。输出不参与缓存，算进去会把数字抬高 |
| `days` | 最近 14 天 `[MM/DD, tokens]`，旧 → 新；超过 14 条设备端自己再截 |
| `avg` | 故意不传。设备端按 `days` 自己算日均，单一实现，免得两边口径漂移 |

关掉：插件配置 `tokens: false`。手动验一包：`node bin/nova-buddy.js test-push --id D93468`。

## 电源关键帧存储与分析（`nova/<id>/up` `evt:"pwr"`）

- 设备仅在关键状态变化（开机黑匣子 `boot`、插拔 USB、充电阶段切换、周期 OCV 探针 `probe`、温控/限流切换、低电阈值）推送 `evt: "pwr"`。
- 宿主侧 `lib/power.js` 将关键帧写入有界环形日志 `$DSH_HOME/nova-buddy/power-events.jsonl`（单文件上限 256 KiB，轮转保留 1 份 `.1.jsonl`，总占用上限 512 KiB），每 30 秒的 `stat` 心跳不写入日志。
- 聚合画像与周期水位线保存在 `$DSH_HOME/nova-buddy/power-summary.json`（每台设备最多保留 60 个充放周期、30 次开机黑匣子、40 次内阻探针）。
- 查看报告或实时抓帧：`node bin/nova-buddy.js power [--id D93468] [--watch 15] [--json]`。关掉落盘：插件配置 `powerLog: false`。

## 测试

```bash
npm test        # node --test
```

`npm install` 之后直接跑，不需要设备或 broker。
