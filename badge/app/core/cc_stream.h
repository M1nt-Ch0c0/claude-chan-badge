// cc_stream.h — 把 Claude Messages API 的流式事件转换成应用回调。
//
// 只关心正文：text 块的 text_delta。thinking 块（Opus 5.5 思考默认不展示，内容为空）
// 和 ping 都被忽略。message_delta 里的 stop_reason 在 message_stop 时一起报告。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_text)(void *ctx, const char *utf8, size_t len);
    // 一条消息结束。stop_reason 可能是 end_turn / max_tokens / refusal 等，未知时为 ""。
    void (*on_done)(void *ctx, const char *stop_reason);
    // 流中出现 error 事件，或数据无法解析。
    void (*on_error)(void *ctx, const char *type, const char *message);
    void *ctx;
} cc_stream_cbs_t;

typedef struct {
    cc_stream_cbs_t cbs;
    char stop_reason[32];
    char model[64];        // message_start 里的实际模型（回退时会变）
    int input_tokens;
    int output_tokens;
    int done;              // 已报告 on_done
    int errored;           // 已报告 on_error
} cc_stream_t;

void cc_stream_init(cc_stream_t *s, const cc_stream_cbs_t *cbs);
// 交给 cc_sse 的事件回调使用：event 为 SSE 事件名，data 为 JSON 文本。
void cc_stream_handle(cc_stream_t *s, const char *event, const char *data);
// 方便直接当作 cc_sse_event_cb 使用（ctx 必须是 cc_stream_t*）。
void cc_stream_sse_cb(void *ctx, const char *event, const char *data);

#ifdef __cplusplus
}
#endif
