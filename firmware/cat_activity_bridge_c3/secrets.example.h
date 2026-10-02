#pragma once

// 复制为 secrets.h 后填写；不要把真实密钥提交到 Git。
constexpr char WIFI_SSID[] = "YOUR_WIFI_NAME";
constexpr char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";

// 直接连接服务器 IP + 端口时使用 false 和 8787。
// 经过你自己的 HTTPS 反向代理时改为 true、域名和 443。
constexpr bool WS_USE_TLS = false;
constexpr char WS_HOST[] = "203.0.113.10";
constexpr uint16_t WS_PORT = 8787;
constexpr char WS_PATH[] = "/ws";

constexpr char CAT_DEVICE_ID[] = "cat-01";
constexpr char CAT_PUBLISH_TOKEN[] = "REPLACE_WITH_SERVER_ENV_TOKEN";
