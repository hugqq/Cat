# Cat WebSocket 服务

这是一个直接由 Node.js + PM2 运行的 WebSocket 服务，不需要 Docker。

## 准备服务器

Linux 服务器需要先安装 Node.js、npm 和 PM2：

```bash
npm install -g pm2
```

然后只把当前 `server` 文件夹复制到服务器。

## 启动

进入服务器上的 `server` 文件夹：

```bash
bash start.sh 8787
```

脚本会自动：

1. 安装生产依赖。
2. 首次运行时生成 ESP32 发布密钥，保存到 `.env`。
3. 使用 PM2 启动服务。
4. 在 systemd 系统上注册开机自启，并保存当前 PM2 进程列表。

非 root 用户首次注册开机自启时，可能需要输入一次 sudo 密码。再次运行 `start.sh` 会保留原来的发布密钥。

## 停止和重启

```bash
bash stop.sh
bash restart.sh
```

更新服务代码后，直接运行 `bash restart.sh`，脚本会重新安装依赖并加载新代码。

常用 PM2 命令：

```bash
pm2 status
pm2 logs cat-state-ws
```

默认监听 `0.0.0.0:8787`：

```text
ws://服务器IP:8787/ws
http://服务器IP:8787/health
```

如果使用域名和 HTTPS，可参考 `nginx-websocket.conf.example` 配置反向代理。配置 TLS 后，客户端地址使用 `wss://你的域名/ws`。
