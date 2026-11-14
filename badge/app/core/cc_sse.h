// cc_sse.h — 增量 Server-Sent Events 解析器（可移植 C，无动态分配）。
//
// 网络层按任意边界把字节喂进来；每凑齐一个事件（空行结束）回调一次。
// 固定大小缓冲，适合无 PSRAM 的 ESP32-C3：单个 data 超过上限时丢弃该事件并计数。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CC_SSE_MAX_LINE  2048
#define CC_SSE_MAX_DATA  2048
#define CC_SSE_MAX_EVENT 48

typedef void (*cc_sse_event_cb)(void *ctx, const char *event, const char *data);

typedef struct {
    char line[CC_SSE_MAX_LINE + 1];
    size_t line_len;
    bool line_overflow;

    char event[CC_SSE_MAX_EVENT + 1];
    char data[CC_SSE_MAX_DATA + 1];
    size_t data_len;
    bool has_data;
    bool data_overflow;

    unsigned dropped_events;  // 因超长被丢弃的事件数
    cc_sse_event_cb cb;
    void *ctx;
} cc_sse_t;

void cc_sse_init(cc_sse_t *p, cc_sse_event_cb cb, void *ctx);
void cc_sse_feed(cc_sse_t *p, const char *bytes, size_t len);
// 流结束时调用：把没有以空行结尾的最后一个事件也派发出去。
void cc_sse_finish(cc_sse_t *p);

#ifdef __cplusplus
}
#endif
