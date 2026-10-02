# 猫咪状态 WebSocket 协议 v1

## 连接地址

设备发布端：

```text
ws://<服务器IP>:8787/ws?device=cat-01&role=device&token=<发布密钥>
```

Electron 查看端：

```text
ws://<服务器IP>:8787/ws?device=cat-01&role=viewer
```

使用自己的 HTTPS 反向代理后，将 `ws://IP:端口` 换成 `wss://域名`。`device` 只能包含字母、数字、下划线和短横线，最长 64 个字符。发布密钥只放在 ESP32-C3 和服务端，Electron 查看端不需要密钥。

## ESP32-C3 上报

```json
{
  "type": "state",
  "protocol": 1,
  "deviceId": "cat-01",
  "state": 3,
  "batteryPercent": 73,
  "seq": 42,
  "sourceMs": 123456
}
```

服务端以连接地址里的 `device` 为准，不信任消息体里的设备编号。

## 服务端广播

```json
{
  "type": "state",
  "protocol": 1,
  "deviceId": "cat-01",
  "state": 3,
  "name": "vigorous_activity",
  "batteryPercent": 73,
  "seq": 42,
  "sourceMs": 123456,
  "updatedAt": "2026-07-23T15:00:00.000Z"
}
```

状态映射：

| state | name |
|---:|---|
| 0 | resting_candidate |
| 1 | walking_candidate |
| 2 | impact_candidate |
| 3 | vigorous_activity |
| 4 | other_activity |
| 255 | warming_up |

`batteryPercent` 为 XIAO 根据单节锂电池电压估算的剩余电量，范围为 `0` 到 `100`。读取无效或使用旧版固件时，服务端广播值为 `null`。电量至少每 60 秒更新一次，状态切换时也会立即附带最新电量。

## 心跳与重连

- 客户端每 20 秒发送文本 `ping`，服务端回复 `pong`。
- ESP32-C3 和 Electron 都在断线后自动重连。
- 服务端会在内存中保存每个设备的最新状态；新打开的 Electron 窗口会立即收到该状态。
