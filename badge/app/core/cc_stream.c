// cc_stream.c — 见 cc_stream.h。
#include "cc_stream.h"

#include <string.h>

#include "cJSON.h"

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void cc_stream_init(cc_stream_t *s, const cc_stream_cbs_t *cbs) {
    memset(s, 0, sizeof(*s));
    if (cbs) s->cbs = *cbs;
}

static void report_error(cc_stream_t *s, const char *type, const char *msg) {
    if (s->errored) return;
    s->errored = 1;
    if (s->cbs.on_error) s->cbs.on_error(s->cbs.ctx, type ? type : "error", msg ? msg : "");
}

void cc_stream_handle(cc_stream_t *s, const char *event, const char *data) {
    (void)event;  // 以 data 里的 type 为准；event 名只用于调试
    cJSON *root = cJSON_Parse(data);
    if (!root) {
        report_error(s, "parse_error", "无法解析流事件 JSON");
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(t, "content_block_delta") == 0) {
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        const cJSON *dtype = cJSON_GetObjectItemCaseSensitive(delta, "type");
        if (cJSON_IsString(dtype) && strcmp(dtype->valuestring, "text_delta") == 0) {
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(delta, "text");
            if (cJSON_IsString(text) && s->cbs.on_text) {
                s->cbs.on_text(s->cbs.ctx, text->valuestring, strlen(text->valuestring));
            }
        }
    } else if (strcmp(t, "message_start") == 0) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        const cJSON *model = cJSON_GetObjectItemCaseSensitive(msg, "model");
        if (cJSON_IsString(model)) copy_str(s->model, sizeof(s->model), model->valuestring);
        const cJSON *usage = cJSON_GetObjectItemCaseSensitive(msg, "usage");
        const cJSON *in = cJSON_GetObjectItemCaseSensitive(usage, "input_tokens");
        if (cJSON_IsNumber(in)) s->input_tokens = in->valueint;
    } else if (strcmp(t, "message_delta") == 0) {
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        const cJSON *sr = cJSON_GetObjectItemCaseSensitive(delta, "stop_reason");
        if (cJSON_IsString(sr)) copy_str(s->stop_reason, sizeof(s->stop_reason), sr->valuestring);
        const cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
        const cJSON *out = cJSON_GetObjectItemCaseSensitive(usage, "output_tokens");
        if (cJSON_IsNumber(out)) s->output_tokens = out->valueint;
    } else if (strcmp(t, "message_stop") == 0) {
        if (!s->done) {
            s->done = 1;
            if (s->cbs.on_done) s->cbs.on_done(s->cbs.ctx, s->stop_reason);
        }
    } else if (strcmp(t, "error") == 0) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *etype = cJSON_GetObjectItemCaseSensitive(err, "type");
        const cJSON *emsg = cJSON_GetObjectItemCaseSensitive(err, "message");
        report_error(s, cJSON_IsString(etype) ? etype->valuestring : "error",
                     cJSON_IsString(emsg) ? emsg->valuestring : "");
    }
    // ping、content_block_start/stop 无需处理。
    cJSON_Delete(root);
}

void cc_stream_sse_cb(void *ctx, const char *event, const char *data) {
    cc_stream_handle((cc_stream_t *)ctx, event, data);
}
