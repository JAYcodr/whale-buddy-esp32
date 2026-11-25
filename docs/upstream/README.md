# 上游基线文档（fork 前，已废弃）

这里是 fork 前的基线文档，来自上游 [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) 与 [vthinkxie 的 ESP32 移植](https://github.com/vthinkxie/claude-desktop-buddy-esp32)：目标形态是 M5StickC Plus + BLE。

当前实现已改为 WiFi + MQTT，协议见 [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md) §3「协议」。

`REFERENCE.md` 描述的 BLE 协议对应 `8ac960d` 提交里的 `src/ble_bridge*`。这些文件已在 `66f2255` 中删除，需要时从 git 历史取回，当前固件不走 BLE 链路。
