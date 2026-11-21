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
