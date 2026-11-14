// cc_bridge.h — USB Serial/JTAG 上的调试与联网桥（配套主机脚本 tools/cc_bridge.py）。
//
// 设备 → 主机（每行以 "@CC " 开头，和普通日志混在同一条串口流里）：
//   @CC HELLO <version>                 开机后每秒一次，直到主机应答
//   @CC POST <id> <path>                开始一个流式 POST
//   @CC H <id> <base64 header line>     请求头（可多行）
//   @CC B <id> <base64 chunk>           请求体分块
//   @CC GO <id> <body_len>              请求体结束，开始请求
//   @CC CANCEL <id>
//   @CC SHOT BEGIN <w> <h> / @CC PX <y> <base64 RGB565LE row> / @CC SHOT END
//   @CC MEM <free> <min_free> <largest>
//
// 主机 → 设备（每行以 '\n' 结束）：
//   @P                          心跳（2 秒一次）；设备据此判断桥是否在线
//   @T <epoch> <posix tz>       设置时间
//   @D <id> <base64 chunk>      响应体分块
//   @E <id> <status> <err>      响应结束
//   @K <up|down|ok> <press|click|double|long|release>   注入按键
//   @S                          截屏
//   @M                          查询堆
//   @R                          重启
//
// API key 始终只在主机脚本的环境变量里，不经过设备。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp_button.h"

typedef void (*cc_bridge_body_cb)(void *user, const uint8_t *data, size_t len);
typedef void (*cc_bridge_done_cb)(void *user, int status, int err);
typedef void (*cc_bridge_key_cb)(bsp_btn_t btn, bsp_btn_ev_t ev);

struct _lv_display_t;

// 安装串口驱动并启动接收任务。on_key 在接收任务里被调用（不持 LVGL 锁）。
bool cc_bridge_start(struct _lv_display_t *disp, cc_bridge_key_cb on_key);
// 最近 5 秒内收到过主机心跳。
bool cc_bridge_alive(void);

// 回调在接收任务里、持有 LVGL 锁时调用。同一时刻只支持一个请求。
int cc_bridge_http_post(const char *path, const char *const *headers, const char *body, size_t body_len,
                        cc_bridge_body_cb on_body, cc_bridge_done_cb on_done, void *user);
void cc_bridge_http_cancel(int id);
