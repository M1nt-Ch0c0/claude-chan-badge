// cc_lan.h — 连到局域网里的 Mac，双向传送 16 kHz PCM。密钥和 Realtime 不在徽章上。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi 已拿到 IP 之后调用。内部重连，失败只打日志。
bool cc_lan_start(void);

// 暂停时不送麦克风，也不把收到的声音送去播放。可从按键任务调用。
void cc_lan_set_paused(bool paused);

// Mac 发来的阶段：0 空闲，1 在听，2 在想，3 在说。
int cc_lan_phase(void);

// Mac 发来的字幕（整段替换）。返回版本号，变了才需要重画。role：0 清空，1 主人，2 小克，3 后台任务。
unsigned cc_lan_text(char *buf, size_t cap, int *role);

// Mac 切换暂停时版本号会变；hold 为当前是否暂停。
unsigned cc_lan_remote_hold(bool *hold);

// 套接字里还没读走的字节数（诊断用）。未连接返回 -1。
int cc_lan_backlog(void);

// Mac 排版好的字幕位图，展开成 A8 写进 a8（cap 至少 w*h 字节）。
// 返回版本号；0 表示还没有或正在写，下一拍再读。role 同上，0 表示隐藏字幕框。
unsigned cc_lan_subimg(uint8_t *a8, size_t cap, int *w, int *h, int *role);
