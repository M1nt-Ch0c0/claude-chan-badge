// cc_sse.c — 见 cc_sse.h。
#include "cc_sse.h"

#include <string.h>

static void reset_event(cc_sse_t *p) {
    p->event[0] = '\0';
    p->data[0] = '\0';
    p->data_len = 0;
    p->has_data = false;
    p->data_overflow = false;
}

void cc_sse_init(cc_sse_t *p, cc_sse_event_cb cb, void *ctx) {
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->ctx = ctx;
}

static void dispatch(cc_sse_t *p) {
    if (p->data_overflow) {
        p->dropped_events++;
    } else if (p->has_data && p->cb) {
        p->cb(p->ctx, p->event[0] ? p->event : "message", p->data);
    }
    reset_event(p);
}

// 处理一行（已去掉行尾 \n 与可能的 \r）。
static void process_line(cc_sse_t *p, const char *line, size_t len) {
    if (len == 0) {
        dispatch(p);
        return;
    }
    if (line[0] == ':') return;  // 注释 / keep-alive

    const char *colon = memchr(line, ':', len);
    size_t name_len = colon ? (size_t)(colon - line) : len;
    const char *value = colon ? colon + 1 : line + len;
    size_t value_len = colon ? len - name_len - 1 : 0;
    if (value_len > 0 && value[0] == ' ') {
        value++;
        value_len--;
    }

    if (name_len == 5 && memcmp(line, "event", 5) == 0) {
        size_t n = value_len < CC_SSE_MAX_EVENT ? value_len : CC_SSE_MAX_EVENT;
        memcpy(p->event, value, n);
        p->event[n] = '\0';
    } else if (name_len == 4 && memcmp(line, "data", 4) == 0) {
        size_t need = value_len + (p->has_data ? 1 : 0);
        if (p->data_overflow || p->data_len + need > CC_SSE_MAX_DATA) {
            p->data_overflow = true;
            p->has_data = true;
            return;
        }
        if (p->has_data) p->data[p->data_len++] = '\n';
        memcpy(p->data + p->data_len, value, value_len);
        p->data_len += value_len;
        p->data[p->data_len] = '\0';
        p->has_data = true;
    }
    // 其余字段（id、retry）对本应用无用，忽略。
}

void cc_sse_feed(cc_sse_t *p, const char *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        if (c == '\n') {
            size_t n = p->line_len;
            if (n > 0 && p->line[n - 1] == '\r') n--;
            if (p->line_overflow) {
                // 超长行：这一行作废，并把所属事件标为溢出。
                p->data_overflow = true;
                p->has_data = true;
            } else {
                p->line[n] = '\0';
                process_line(p, p->line, n);
            }
            p->line_len = 0;
            p->line_overflow = false;
            continue;
        }
        if (p->line_len < CC_SSE_MAX_LINE) {
            p->line[p->line_len++] = c;
        } else {
            p->line_overflow = true;
        }
    }
}

void cc_sse_finish(cc_sse_t *p) {
    if (p->line_len > 0) {
        size_t n = p->line_len;
        if (n > 0 && p->line[n - 1] == '\r') n--;
        if (!p->line_overflow) {
            p->line[n] = '\0';
            process_line(p, p->line, n);
        }
        p->line_len = 0;
        p->line_overflow = false;
    }
    if (p->has_data) dispatch(p);
}
