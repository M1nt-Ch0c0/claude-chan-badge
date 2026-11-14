// 复制成 wifi_secret.h 再填（wifi_secret.h 不进 git）。
#pragma once

#define CC_WIFI_SSID     "your-wifi"
#define CC_WIFI_PASSWORD "your-password"

// 跑 VoiceDesk 的那台 Mac 的局域网地址。还没做自动发现。
#define CC_LAN_HOST      "192.168.1.10"
#define CC_LAN_PORT      19731

// 下面三项只有文字对话（不开语音、徽章自己发 HTTPS）才用得到，语音模式可以留空。
#define CC_API_BASE      "https://api.anthropic.com"
#define CC_API_KEY       ""
// #define CC_MODEL      "claude-sonnet-5-5"
