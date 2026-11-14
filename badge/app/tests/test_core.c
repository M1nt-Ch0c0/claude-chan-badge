// test_core.c — Claude娘 核心逻辑的主机单元测试（无需模拟器、无需网络）。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cc_chat.h"
#include "cc_reply.h"
#include "cc_sse.h"
#include "cc_stream.h"

static int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

// ---- 收集回调结果 ---------------------------------------------------------

typedef struct {
    char text[4096];
    size_t text_len;
    char stop[32];
    int done;
    char err[128];
    // reply
    int emotion;
    int emotion_calls;
    char body[4096];
    size_t body_len;
    char opts[CC_MAX_OPTIONS][CC_OPTION_BYTES + 1];
    int n_opts;
    int bad_utf8_chunk;
} sink_t;

static int utf8_chunk_ok(const char *s, size_t n) {
    // 每一块都必须由完整的 UTF-8 字符组成
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t need = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (need == 0 || i + need > n) return 0;
        for (size_t k = 1; k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
        }
        i += need;
    }
    return 1;
}

static void on_text(void *ctx, const char *t, size_t n) {
    sink_t *s = ctx;
    memcpy(s->text + s->text_len, t, n);
    s->text_len += n;
    s->text[s->text_len] = 0;
}
static void on_done(void *ctx, const char *stop) {
    sink_t *s = ctx;
    s->done++;
    snprintf(s->stop, sizeof(s->stop), "%s", stop);
}
static void on_error(void *ctx, const char *type, const char *msg) {
    sink_t *s = ctx;
    snprintf(s->err, sizeof(s->err), "%s:%s", type, msg);
}
static void on_emotion(void *ctx, cc_emotion_t e) {
    sink_t *s = ctx;
    s->emotion = e;
    s->emotion_calls++;
}
static void on_body(void *ctx, const char *t, size_t n) {
    sink_t *s = ctx;
    if (!utf8_chunk_ok(t, n)) s->bad_utf8_chunk = 1;
    memcpy(s->body + s->body_len, t, n);
    s->body_len += n;
    s->body[s->body_len] = 0;
}
static void on_option(void *ctx, int i, const char *t) {
    sink_t *s = ctx;
    snprintf(s->opts[i], sizeof(s->opts[i]), "%s", t);
    s->n_opts = i + 1;
}

// ---- 1. SSE + 流事件 ------------------------------------------------------

static const char *SSE_STREAM =
    "event: message_start\r\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
    "\"model\":\"claude-opus-5-5\",\"content\":[],\"usage\":{\"input_tokens\":321,\"output_tokens\":1}}}\r\n"
    "\r\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"abc\"}}\n"
    "\n"
    ": keep-alive comment\n"
    "event: ping\n"
    "data: {\"type\": \"ping\"}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"[开心]\\n早上\"}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"好呀\\u0021 \\\"引号\\\"\"}}\n"
    "\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":42}}\n"
    "\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n"
    "\n";

static void test_sse_stream_all_splits(void) {
    size_t total = strlen(SSE_STREAM);
    for (size_t chunk = 1; chunk <= total; chunk++) {
        sink_t sink = {0};
        cc_stream_t st;
        cc_stream_cbs_t cbs = {on_text, on_done, on_error, &sink};
        cc_stream_init(&st, &cbs);
        cc_sse_t sse;
        cc_sse_init(&sse, cc_stream_sse_cb, &st);
        for (size_t off = 0; off < total; off += chunk) {
            size_t n = total - off < chunk ? total - off : chunk;
            cc_sse_feed(&sse, SSE_STREAM + off, n);
        }
        cc_sse_finish(&sse);
        CHECK(strcmp(sink.text, "[开心]\n早上好呀! \"引号\"") == 0);
        CHECK(sink.done == 1);
        CHECK(strcmp(sink.stop, "end_turn") == 0);
        CHECK(sink.err[0] == 0);
        CHECK(strcmp(st.model, "claude-opus-5-5") == 0);
        CHECK(st.input_tokens == 321);
        CHECK(st.output_tokens == 42);
    }
}

static void test_sse_error_event(void) {
    const char *s =
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n";
    sink_t sink = {0};
    cc_stream_t st;
    cc_stream_cbs_t cbs = {on_text, on_done, on_error, &sink};
    cc_stream_init(&st, &cbs);
    cc_sse_t sse;
    cc_sse_init(&sse, cc_stream_sse_cb, &st);
    cc_sse_feed(&sse, s, strlen(s));
    CHECK(strcmp(sink.err, "overloaded_error:Overloaded") == 0);
    CHECK(sink.done == 0);
}

static void test_sse_no_trailing_blank_line(void) {
    const char *s = "data: {\"type\":\"message_stop\"}";
    sink_t sink = {0};
    cc_stream_t st;
    cc_stream_cbs_t cbs = {on_text, on_done, on_error, &sink};
    cc_stream_init(&st, &cbs);
    cc_sse_t sse;
    cc_sse_init(&sse, cc_stream_sse_cb, &st);
    cc_sse_feed(&sse, s, strlen(s));
    CHECK(sink.done == 0);
    cc_sse_finish(&sse);
    CHECK(sink.done == 1);
}

static void test_sse_overflow_dropped(void) {
    static char big[CC_SSE_MAX_DATA + 200];
    memcpy(big, "data: ", 6);
    memset(big + 6, 'x', sizeof(big) - 8);
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = '\n';
    sink_t sink = {0};
    cc_stream_t st;
    cc_stream_cbs_t cbs = {on_text, on_done, on_error, &sink};
    cc_stream_init(&st, &cbs);
    cc_sse_t sse;
    cc_sse_init(&sse, cc_stream_sse_cb, &st);
    cc_sse_feed(&sse, big, sizeof(big));
    CHECK(sse.dropped_events == 1);
    const char *ok = "data: {\"type\":\"message_stop\"}\n\n";
    cc_sse_feed(&sse, ok, strlen(ok));
    CHECK(sink.done == 1);  // 溢出之后仍能继续解析
}

// ---- 2. 回复格式 ----------------------------------------------------------

static void run_reply(const char *text, size_t chunk, sink_t *sink) {
    memset(sink, 0, sizeof(*sink));
    sink->emotion = -1;
    cc_reply_t r;
    cc_reply_cbs_t cbs = {on_emotion, on_body, on_option, sink};
    cc_reply_init(&r, &cbs);
    size_t total = strlen(text);
    for (size_t off = 0; off < total; off += chunk) {
        size_t n = total - off < chunk ? total - off : chunk;
        cc_reply_feed(&r, text + off, n);
    }
    cc_reply_finish(&r);
}

static void test_reply_all_splits(void) {
    const char *text =
        "[开心]\n"
        "早上好呀！今天电量满满。\n"
        "\n"
        "想聊点什么？\n"
        ">今天有什么安排\n"
        ">讲个冷笑话 \r\n"
        "＞ 摸摸头\n";
    size_t total = strlen(text);
    for (size_t chunk = 1; chunk <= total; chunk++) {
        sink_t s;
        run_reply(text, chunk, &s);
        CHECK(s.emotion == CC_EMO_HAPPY);
        CHECK(s.emotion_calls == 1);
        CHECK(strcmp(s.body, "早上好呀！今天电量满满。\n\n想聊点什么？") == 0);
        CHECK(s.bad_utf8_chunk == 0);
        CHECK(s.n_opts == 3);
        CHECK(strcmp(s.opts[0], "今天有什么安排") == 0);
        CHECK(strcmp(s.opts[1], "讲个冷笑话") == 0);
        CHECK(strcmp(s.opts[2], "摸摸头") == 0);
    }
}

static void test_reply_variants(void) {
    sink_t s;
    // 没有标签：平静，正文完整保留
    run_reply("你好，我是 Claude娘。\n>你好", 3, &s);
    CHECK(s.emotion == CC_EMO_CALM);
    CHECK(strcmp(s.body, "你好，我是 Claude娘。") == 0);
    CHECK(s.n_opts == 1);

    // 全角括号标签、标签和正文同一行、未知表情
    run_reply("【害羞】  别、别一直盯着我看嘛……", 2, &s);
    CHECK(s.emotion == CC_EMO_SHY);
    CHECK(strcmp(s.body, "别、别一直盯着我看嘛……") == 0);
    run_reply("[发呆]\n嗯？", 1, &s);
    CHECK(s.emotion == CC_EMO_CALM);
    CHECK(strcmp(s.body, "嗯？") == 0);

    // 以 [ 开头但不是标签（太长）：当正文显示
    run_reply("[这是一个非常非常非常非常长的方括号开头的句子没有闭合\n下一行", 5, &s);
    CHECK(s.emotion == CC_EMO_CALM);
    CHECK(strstr(s.body, "下一行") != NULL);

    // 超过 3 个选项只取前 3 个；选项区里的杂行被忽略
    run_reply("[思考]\n嗯……\n>一\n>二\n备注：随便\n>三\n>四", 4, &s);
    CHECK(s.n_opts == 3);
    CHECK(strcmp(s.opts[2], "三") == 0);
    CHECK(strcmp(s.body, "嗯……") == 0);

    // 标签前漏出的分析行：在前 3 行内遇到合法表情就丢掉杂行
    const char *leak = "Hmm, the assistant should follow format.\n\n[害羞]\n唔……屏幕都要发烫啦。\n>再摸一下";
    for (size_t chunk = 1; chunk <= strlen(leak); chunk++) {
        run_reply(leak, chunk, &s);
        CHECK(s.emotion == CC_EMO_SHY);
        CHECK(s.emotion_calls == 1);
        CHECK(strcmp(s.body, "唔……屏幕都要发烫啦。") == 0);
        CHECK(s.n_opts == 1);
    }
    // 第二行的方括号不是已知表情：不当标签，全部作为正文
    run_reply("今天读了一本书。\n[注意]这本书很长\n>好", 3, &s);
    CHECK(s.emotion == CC_EMO_CALM);
    CHECK(strcmp(s.body, "今天读了一本书。\n[注意]这本书很长") == 0);
    CHECK(s.n_opts == 1);

    // 有效性判断
    {
        cc_reply_t r;
        cc_reply_cbs_t none = {0};
        cc_reply_init(&r, &none);
        const char *garbage = "\xd0\xb5\xd1\x81\xd1\x82\xd0\xb8";  // "ести"
        cc_reply_feed(&r, garbage, strlen(garbage));
        cc_reply_finish(&r);
        CHECK(!cc_reply_looks_valid(&r, strlen(garbage)));
        cc_reply_init(&r, &none);
        const char *good = "[开心]\n好呀好呀\n>一\n>二\n>三";
        cc_reply_feed(&r, good, strlen(good));
        cc_reply_finish(&r);
        CHECK(cc_reply_looks_valid(&r, strlen("好呀好呀")));
    }

    // 空回复
    run_reply("", 1, &s);
    CHECK(s.emotion == CC_EMO_CALM);
    CHECK(s.body_len == 0);
    CHECK(s.n_opts == 0);

    // 正文以 > 以外的全角字符开头（与 ＞ 共享首字节 0xEF 的字符，例如 （ = EF BC 88）
    run_reply("[平静]\n（伸了个懒腰）\n>早", 1, &s);
    CHECK(strcmp(s.body, "（伸了个懒腰）") == 0);
    CHECK(s.n_opts == 1);
}

// ---- 3. 历史与请求 ---------------------------------------------------------

static void test_history_and_request(void) {
    cc_history_t h;
    cc_history_init(&h);
    char line[256];
    for (int i = 0; i < 20; i++) {
        snprintf(line, sizeof(line), "第 %d 句：%s", i, i % 2 ? "回答回答回答" : "问题问题");
        CHECK(cc_history_push(&h, i % 2 ? CC_ROLE_ASSISTANT : CC_ROLE_USER, line));
        CHECK(h.count <= CC_HISTORY_MAX_MSGS);
        CHECK(h.bytes <= CC_HISTORY_MAX_BYTES);
        CHECK(h.msgs[0].role == CC_ROLE_USER);
    }
    // 字节上限
    static char big[1500];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    for (int i = 0; i < 6; i++) CHECK(cc_history_push(&h, i % 2 ? CC_ROLE_ASSISTANT : CC_ROLE_USER, big));
    CHECK(h.bytes <= CC_HISTORY_MAX_BYTES);
    CHECK(h.msgs[0].role == CC_ROLE_USER);

    cc_history_clear(&h);
    CHECK(cc_history_push(&h, CC_ROLE_USER, "你好"));
    char opts[3][CC_OPTION_BYTES + 1] = {"一", "二", "三"};
    char *canon = cc_reply_canonical(CC_EMO_PROUD, "哼哼，我可是很厉害的。", opts, 3);
    CHECK(strcmp(canon, "[得意]\n哼哼，我可是很厉害的。\n>一\n>二\n>三") == 0);
    CHECK(cc_history_push(&h, CC_ROLE_ASSISTANT, canon));
    free(canon);
    CHECK(cc_history_push(&h, CC_ROLE_USER, "（摸了摸你的头）"));

    cc_req_cfg_t cfg = {"claude-opus-5-5", 600, "low", true};
    cc_device_ctx_t dev = {15, true, "周六 14:05"};
    char *json = cc_req_build(&cfg, &h, &dev);
    CHECK(json != NULL);
    cJSON *root = cJSON_Parse(json);
    CHECK(root != NULL);
    CHECK(strcmp(cJSON_GetObjectItem(root, "model")->valuestring, "claude-opus-5-5") == 0);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(root, "stream")));
    CHECK(strcmp(cJSON_GetObjectItem(root, "fallbacks")->valuestring, "default") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "output_config"), "effort")->valuestring, "low") == 0);
    CHECK(cJSON_GetObjectItem(root, "temperature") == NULL);
    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    CHECK(cJSON_GetArraySize(msgs) == 3);
    const char *last = cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 2), "content")->valuestring;
    CHECK(strstr(last, "电量 15%，正在充电") != NULL);
    CHECK(strstr(last, "（摸了摸你的头）") != NULL);
    cJSON_Delete(root);
    free(json);

    // 没有任何设备信息时不加设备前缀
    cc_device_ctx_t none = {-1, false, NULL};
    json = cc_req_build(&cfg, &h, &none);
    root = cJSON_Parse(json);
    msgs = cJSON_GetObjectItem(root, "messages");
    last = cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 2), "content")->valuestring;
    CHECK(strcmp(last, "（摸了摸你的头）") == 0);
    cJSON_Delete(root);
    free(json);

    // 最后一条不是 user：拒绝构建
    cc_history_pop(&h);
    CHECK(cc_req_build(&cfg, &h, &dev) == NULL);
    cc_history_clear(&h);
}

int main(void) {
    test_sse_stream_all_splits();
    test_sse_error_event();
    test_sse_no_trailing_blank_line();
    test_sse_overflow_dropped();
    test_reply_all_splits();
    test_reply_variants();
    test_history_and_request();
    printf("claude_chan core: %d checks passed\n", g_checks);
    return 0;
}
