// cc_cli.c — 主机端调试工具：用 Claude娘 的核心代码构建请求、解析流式响应。
//
//   cc_cli build "用户的话" [model]   -> 把请求 JSON 打到 stdout
//   cc_cli parse                      -> 从 stdin 读 SSE，打印表情、正文、选项和停止原因
//
// 网络请求交给 curl（见 tests/live_chat.sh），本工具不接触 API key。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc_chat.h"
#include "cc_reply.h"
#include "cc_sse.h"
#include "cc_stream.h"

typedef struct {
    cc_reply_t reply;
    char body[2048];
    size_t body_len;
} cli_t;

static void on_text(void *ctx, const char *t, size_t n) { cc_reply_feed(&((cli_t *)ctx)->reply, t, n); }
static void on_done(void *ctx, const char *stop) {
    cli_t *c = ctx;
    cc_reply_finish(&c->reply);
    printf("\n[stop_reason] %s\n", stop[0] ? stop : "(none)");
}
static void on_error(void *ctx, const char *type, const char *msg) {
    (void)ctx;
    printf("\n[error] %s: %s\n", type, msg);
}
static void on_emotion(void *ctx, cc_emotion_t e) {
    (void)ctx;
    printf("[emotion] %s\n[body] ", cc_emotion_name(e));
    fflush(stdout);
}
static void on_body(void *ctx, const char *t, size_t n) {
    (void)ctx;
    fwrite(t, 1, n, stdout);
    fflush(stdout);
}
static void on_option(void *ctx, int i, const char *t) {
    (void)ctx;
    printf("\n[option %d] %s", i + 1, t);
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "build") == 0) {
        cc_history_t h;
        cc_history_init(&h);
        cc_history_push(&h, CC_ROLE_USER, argv[2]);
        cc_req_cfg_t cfg = {argc >= 4 ? argv[3] : "claude-opus-5-5", 600, "low", true};
        cc_device_ctx_t dev = {76, false, "周六 10:30"};
        char *json = cc_req_build(&cfg, &h, &dev);
        if (!json) return 1;
        fputs(json, stdout);
        free(json);
        cc_history_clear(&h);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "parse") == 0) {
        static cli_t cli;
        cc_reply_cbs_t rc = {on_emotion, on_body, on_option, &cli};
        cc_reply_init(&cli.reply, &rc);
        static cc_stream_t st;
        cc_stream_cbs_t sc = {on_text, on_done, on_error, &cli};
        cc_stream_init(&st, &sc);
        static cc_sse_t sse;
        cc_sse_init(&sse, cc_stream_sse_cb, &st);
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) cc_sse_feed(&sse, buf, n);
        cc_sse_finish(&sse);
        printf("[model] %s  [tokens] in=%d out=%d\n", st.model, st.input_tokens, st.output_tokens);
        return st.errored ? 2 : 0;
    }
    fprintf(stderr, "usage: cc_cli build TEXT [MODEL] | cc_cli parse\n");
    return 64;
}
