# 机内语音条：生成 / 替换 / 新增 / 上机

> **读者**：AI 与改动者。换一段语音、加一条台词、或者"点了没声音"时，照这一份做，别猜。
> **相关**：[`tools/assets/gen_voice.mjs`](../tools/assets/gen_voice.mjs) · [`host-plugin/lib/adpcm.js`](../host-plugin/lib/adpcm.js) · [`ARCHITECTURE.md`](ARCHITECTURE.md) §4.2
> **上一轮实际案例**：2026-11-26 用本文流程把 12 条点击/唤醒语音换成 `小孩_女` 参考音（commit `9a11c17` 之前那批）。

---

## 0. 链路一句话

听书值守的**本地 Qwen3-TTS** 克隆参考音合成 WAV → `gen_voice.mjs` 做响度 → `adpcm.js` 转 **NVAD 4-bit ADPCM（16kHz）** → 写 `assets/audio/<name>.adp` → `prep_data.sh` 拷进 `data/` → 烧 LittleFS → 设备 `sfxSpeakFile("/audio/<name>.adp")` 播放。

设备端**只认文件**，不联网合成。

## 1. 先确认环境

| 项 | 怎么看 |
|---|---|
| 听书值守在跑 | `curl -s http://127.0.0.1:<server.port>/health`，或看 DSH 面板；端口在 `~/.dsh/plugin-data/mimo-tts/config.json` 的 `server.port`（默认 8292） |
| 用的模型 | `upstream.local.modelPath`，当前 `qwen3tts-0.6b-base-8bit` |
| 参考音 | `upstream.local.refs.narrator.wav` + `.text`（text 必须**逐字**是 wav 里那句话的转写） |
| 设备 | `bash tools/pio.sh device list` 能看到 `/dev/cu.usbmodem*` |

`gen_voice.mjs` 自己读配置里的端口和 token，DSH 重启换端口不影响。

## 2. 换参考音（要动配置，用完必须还原）

**关键坑**：本地 provider 的参考音取**已保存的配置**；`/api/preview` 的 `draft` 只覆盖 `provider`，覆盖不了 `refs`。所以换音色必须临时改配置。

1. 备份：`cp ~/.dsh/plugin-data/mimo-tts/config.json /tmp/config.backup.json`
2. 分字段写（**不要**一次 set 整个 `refs.narrator` 对象，校验会把 wav/text 判成空）：

   ```bash
   # 通过 DSH 工具 audiobook_tts_config，路径 + 值分开写
   # upstream.local.refs.narrator.wav  = /path/to/ref.wav
   # upstream.local.refs.narrator.text = "参考音里说的那句话"
   ```

3. 当前可用的参考音：`/Users/hamelin/Desktop/有声书参考音频/小孩_女/archive_huohuo_15.wav`（15.9s，转写在同名 `.lab`）。**不要把它提交进仓库**（版权）。
4. 生成完把 wav / text **两项都改回去**；改完对比解析后的 JSON 与备份是否一致。
5. 插件的写入路径会顺手补一个默认键 `synth.dialogueQuoteChars`；要"完全还原"就删掉这个键再比。

## 3. 生成 / 替换

```bash
node tools/assets/gen_voice.mjs --list          # 看 key 与台词
node tools/assets/gen_voice.mjs                 # 全部 12 条，约 30 秒
node tools/assets/gen_voice.mjs wake_0          # 只重生成一条
```

脚本内部固定：`provider=local`（draft 强制）、参考音取配置、响度 **RMS −20 dB + 峰值限 −7 dB**（`alimiter=...,level=0`；不开 level=0 会自动拉满 0 dB，小喇叭会破）。

### 加一条新台词（同变体、第 2 条以后）

1. `tools/assets/gen_voice.mjs` 的 `CLIPS` 加一行 `['click_shy_2', '……']`；
2. 当前代码只认 `<variant>_0|1`：`page_home.cpp` 的 `s_quoteIdx % 2` 与 `main.cpp` 的 `act.aux & 1` 需要改成 `% N`，文件名跟着排；
3. 生成、打包、烧录。

### 加一个新变体（比如"吃饭"动作配语音）

1. `assets/audio/<variant>_0.adp` / `_1.adp` 落地；
2. `main.cpp` 的 `voiceBase` switch 加分支；
3. `page_home.cpp` 对应动作返回 `UiAction{ PET_TAP, 0, (int)anim, quoteIdx }`；
4. 版本兼容：**保留旧 `tap0-2` 作回退**，老 LittleFS 没刷时不会哑。

## 4. 校验（不用上机）

```bash
node tools/assets/nvad_decode.mjs assets/audio/wake_0.adp /tmp/wake_0.wav
node tools/assets/nvad_decode.mjs assets/audio/*.adp --out /tmp/nova-voice

ffmpeg -hide_banner -nostats -i /tmp/wake_0.wav -af astats=metadata=1:reset=0 -f null -
node tools/asr_probe.mjs /tmp/wake_0.wav      # ASR 回读对拍，需 XIAOMI_API_KEY
```

合格线：RMS 在 −20±1 dB、峰值 ≥ −8 dB；ASR 回读与台词一致；文件 ≤ ~30KB（超过 3.5s 说明断句/文本有问题）。

## 5. 打包与上机

```bash
bash tools/prep_data.sh                 # assets/audio → data/audio（data/ 是 gitignore，不要提交）
bash tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8 -t uploadfs
bash tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8 -t upload
```

只刷 `uploadfs` 也能换声音（代码没变时）。真机验证：点宠物、8 秒内连戳三下（生气）、戳醒打盹的它（唤醒）。

## 6. 坑

| 坑 | 后果 |
|---|---|
| 把 1.7B 或别的参考音留在配置里 | 读书音色被换掉；生成前/后都要核对 |
| 参考音 text 与 wav 不一致 | 克隆跑偏，音色和语气都可能不对 |
| 用 `draft` 传 refs 想免改配置 | 不生效，输出会用旧参考音（代码里 local provider 读 `getConfig()`） |
| 忽略响度 | 小喇叭削波，听起来"破" |
| 只改 assets 不刷 LittleFS | 设备读不到新文件，回退 `tap0-2`（能响但不是新音色） |
| `base` 模型想设情绪 | 不支持：`generate()` 的 `instruct` 只对 CustomVoice/VoiceDesign 生效，base 走 `_generate_icl()` 没有入口；情绪只能靠参考音或文本语气 |
| 把参考音 / 候选 wav 提交进仓库 | 版权与体积问题；只提交 `assets/audio/*.adp` |
