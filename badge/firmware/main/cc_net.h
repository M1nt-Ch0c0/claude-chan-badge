// cc_net.h — 真机 Wi-Fi 直连和 HTTPS 流式 POST。凭据在 gitignore 的 wifi_secret.h 里。
#pragma once

#include <stdbool.h>

#include "cc_platform.h"

// 连接家里的 2.4 GHz 热点。超时返回 false，应用仍可走 USB 桥。
bool cc_wifi_start(int timeout_ms);
bool cc_wifi_ready(void);

int cc_https_post(const char *path, const char *const *headers, const char *body, size_t body_len,
                  cc_http_body_cb on_body, cc_http_done_cb on_done, void *user);
void cc_https_cancel(int id);
