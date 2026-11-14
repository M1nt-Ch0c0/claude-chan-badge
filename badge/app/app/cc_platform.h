// cc_platform.h — Claude娘 应用对平台的全部依赖。
//
// 应用代码（claude_chan_app.c、cc_avatar.c 和 core/）只通过这里访问平台。
// 模拟器实现在 platform/cc_platform_sim.c；真机以后提供 cc_platform_idf.c：
//   - HTTP 换成 esp_http_client（流式读，回调投递到 LVGL 任务）
//   - 字体换成 flash 中的同一份 TTF + TinyTTF
//   - 电量用 bsp_battery_soc()，充电用 CW2017 电量上升或 USB 供电检测
//   - 鉴权由中转服务（例如自己搭的中转）持有，设备只带可吊销的设备令牌
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 回调保证在 LVGL 所在的线程/任务上执行，可以直接改 UI。
typedef void (*cc_http_body_cb)(void *user, const uint8_t *data, size_t len);
// status: HTTP 状态码（未拿到响应时为 0）；err: 0 表示传输成功。
typedef void (*cc_http_done_cb)(void *user, int status, int err);

// 发起流式 POST。path 以 "/" 开头，由平台拼接中转服务地址并注入鉴权。
// headers 为 NULL 结尾的 "Name: value" 数组。body 在函数返回前被拷走。
// 返回请求 id（>=0），失败返回负数。
int cc_platform_http_post(const char *path, const char *const *headers, const char *body, size_t body_len,
                          cc_http_body_cb on_body, cc_http_done_cb on_done, void *user);
void cc_platform_http_cancel(int id);

// CJK 字体（同一字号只创建一次，平台负责缓存与释放）。
const lv_font_t *cc_platform_font(int px);

int cc_platform_battery_soc(void);  // 0..100，未知返回 -1
bool cc_platform_charging(void);

// 应用的大块内存（计入模拟器的内存预算；真机即 malloc/free）。
void *cc_platform_alloc(size_t n);
void cc_platform_free(void *p);

// 对话用的模型名（模拟器从环境变量 PASSPORT_MODEL 读，默认 claude-opus-5-5）。
const char *cc_platform_model(void);

// 暂停或恢复局域网语音。模拟器里是空操作。
void cc_platform_voice_hold(bool hold);
// 0 空闲，1 在听，2 在想，3 在说。
int cc_platform_voice_phase(void);
// 语音字幕：Mac 发来的最新一段（整段替换，已截到最后几行）。返回版本号，变了才需要重画。
// role：0 清空，1 主人说的，2 小克说的，3 后台任务进度。
unsigned cc_platform_voice_text(char *buf, size_t cap, int *role);
// Mac 那边切换暂停时版本号会变，hold 是当前是否暂停。
unsigned cc_platform_voice_remote_hold(bool *hold);
// Mac 排版好的字幕位图（A8，宽 w 高 h，写进 a8）。返回版本号；0 表示没有或正在更新，下一拍再取。
// 有位图时应用就不用自己排字，也就不占字形缓存。老的 Mac 端只发文字，走上面那条。
unsigned cc_platform_voice_subimg(uint8_t *a8, size_t cap, int *w, int *h, int *role);

#ifdef __cplusplus
}
#endif
