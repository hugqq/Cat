# ESP32-C3 SuperMini 网关

`cat_activity_bridge_c3.ino` 同时完成两件事：

1. 通过 BLE 订阅 `CatMotion-01` 的 16 字节分类结果。
2. 通过 Wi-Fi 与自建的 `ws://IP:端口` 服务保持长连接并上报状态。

XIAO 会通过板载分压电路估算单节锂电池剩余电量，并复用 16 字节分类包的原预留字节传给网关。网关在状态切换时上报电量，状态不变时每 60 秒上报一次，因此不会恢复频繁换图。

为避免桌面猫咪频繁切换图片，运动端每 2 秒生成一次分类结果；网关只在新状态连续出现 3 次后确认切换，相同状态不会重复上报。底层 IMU 仍保持 50Hz 采样，不改变原有识别特征。

为增强隔墙连接，XIAO nRF52840 的 BLE 发射功率设为 +8 dBm，ESP32-C3 的扫描、建连和连接发射功率设为 +9 dBm。提高发射功率会增加耗电，并且不能完全消除墙体、猫身体和金属对 2.4GHz 信号的遮挡。

## 准备

Arduino IDE 中安装：

- Espressif 的 `esp32` 开发板支持包
- Library Manager 中的 `WebSockets`（作者 Markus Sattler）

开发板选择 `ESP32C3 Dev Module`，并把 `USB CDC On Boot` 设为 `Enabled`。

复制 `secrets.example.h` 为 `secrets.h`，填写 Wi-Fi、服务器 IP、端口、设备编号和发布密钥，然后上传。

发布密钥必须与服务器 `server/.env` 里的 `CAT_PUBLISH_TOKEN` 完全一致。如果以后使用自己的 HTTPS 反向代理，把 `WS_USE_TLS` 改成 `true`、端口改成 `443`，主机名填写域名。
