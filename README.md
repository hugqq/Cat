# 猫咪桌面宠物

完整链路：

```text
XIAO nRF52840 Sense
  └─ BLE 分类结果
      └─ ESP32-C3 SuperMini
          └─ Wi-Fi / WebSocket
              └─ 自建 Linux WS 服务（IP + 端口）
                  └─ Electron 透明桌面宠物
```

## 1. 启动 Electron 桌面宠物

PowerShell 7：

```powershell
npm install
npm run electron:dev
```

首次启动填写：

```text
ws://你的服务器IP:8787/ws
```

配置完成后窗口只显示猫咪 GIF：

- 拖动猫咪上方的透明区域可以移动窗口。
- 双击猫咪重新打开连接设置。
- 右键猫咪可以重新连接、调整大小、切换置顶或退出。
- 猫咪右下角会显示 XIAO 估算剩余电量；25% 以下显示黄色，10% 以下显示红色。
- 窗口会记住上次的位置和大小。

生成 Windows 单文件程序：

```powershell
npm run electron:pack
```

生成结果在 `desktop-release/`。

## 2. 部署 WS 服务

服务器只需要 Node.js、npm 和 PM2，不需要 Docker。把 `server` 文件夹复制到 Linux 服务器，进入该文件夹后执行：

```bash
bash start.sh 8787
```

脚本会安装依赖、生成并保存 ESP32 发布密钥、启动服务，并注册 PM2 开机自启。

停止和重启：

```bash
bash stop.sh
bash restart.sh
```

详细说明和 Nginx 反向代理示例见 `server/README.md`。

## 3. ESP32-C3 配置

复制：

```text
firmware/cat_activity_bridge_c3/secrets.example.h
```

为：

```text
firmware/cat_activity_bridge_c3/secrets.h
```

填写 Wi-Fi、服务器 IP、端口和 `server/.env` 中的发布密钥。详细说明见 `firmware/README.md`。

## 状态映射

| 编号 | 状态 | GIF |
|---:|---|---|
| 0 | resting_candidate | 静息 |
| 1 | walking_candidate | 行走 |
| 2 | impact_candidate | 碰撞 |
| 3 | vigorous_activity | 剧烈活动 |
| 4 | other_activity | 其他活动 |

通信消息格式见 `docs/websocket-protocol.md`。
