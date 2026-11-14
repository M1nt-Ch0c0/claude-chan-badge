// cc_chat.h — Claude娘 的对话历史与 Messages API 请求构建。
//
// 设备内存很紧，历史按条数和总字节两个上限裁剪，总是成对（user+assistant）丢弃最旧的。
// 请求体用 cJSON 生成（ESP-IDF 自带 cJSON，真机可原样使用）。
// API key 不在这里出现：由宿主（模拟器 / 中转服务）注入请求头。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cc_reply.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CC_HISTORY_MAX_MSGS   12     // 最多 6 轮
#define CC_HISTORY_MAX_BYTES  3072   // 所有消息正文的总字节上限

typedef enum { CC_ROLE_USER = 0, CC_ROLE_ASSISTANT = 1 } cc_role_t;

typedef struct {
    cc_role_t role;
    char *text;  // malloc 所得，cc_history_clear 释放
} cc_msg_t;

typedef struct {
    cc_msg_t msgs[CC_HISTORY_MAX_MSGS];
    int count;
    size_t bytes;
} cc_history_t;

void cc_history_init(cc_history_t *h);
void cc_history_clear(cc_history_t *h);
// 追加一条消息（会复制 text）。超出上限时成对丢弃最旧的消息。返回 false 表示内存不足。
bool cc_history_push(cc_history_t *h, cc_role_t role, const char *text);
// 删除最后一条（例如请求失败时撤回刚追加的 user 消息）。
void cc_history_pop(cc_history_t *h);

// 把一次解析好的回复还原成规范文本存进历史，让模型看到一致的格式。
// 返回 malloc 的字符串，调用方释放。
char *cc_reply_canonical(cc_emotion_t emo, const char *body, char options[][CC_OPTION_BYTES + 1], int n_options);

typedef struct {
    const char *model;      // 例如 "claude-opus-5-5"
    int max_tokens;         // 例如 600
    const char *effort;     // "low" / "medium" / ...；NULL 表示不设置
    bool server_fallbacks;  // 是否带 fallbacks:"default"（需要对应 beta 头）
} cc_req_cfg_t;

typedef struct {
    int battery_soc;        // 0..100，-1 未知
    bool charging;
    const char *local_time; // 例如 "周六 14:05"，可为 NULL
} cc_device_ctx_t;

// 构建请求体。最后一条消息取历史末尾（必须是 user）。返回 malloc 的 JSON 字符串。
char *cc_req_build(const cc_req_cfg_t *cfg, const cc_history_t *h, const cc_device_ctx_t *dev);

// 请求需要的固定请求头（不含鉴权）。以 NULL 结尾的 "Name: value" 数组。
extern const char *const CC_REQ_HEADERS[];
extern const char *const CC_REQ_PATH;  // "/v1/messages"

// 系统提示（人设 + 输出格式），只读常量。
extern const char *const CC_SYSTEM_PROMPT;

#ifdef __cplusplus
}
#endif
