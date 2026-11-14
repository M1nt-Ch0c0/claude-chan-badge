// cc_chat.c — 见 cc_chat.h。
#include "cc_chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

const char *const CC_REQ_PATH = "/v1/messages";

const char *const CC_REQ_HEADERS[] = {
    "content-type: application/json",
    "anthropic-version: 2026-10-21",
    "anthropic-beta: server-side-fallback-2026-07-01",
    NULL,
};

const char *const CC_SYSTEM_PROMPT =
    "你是「Claude娘」，住在一块名叫 AI Passport 的可穿戴小徽章里的 AI 伙伴。"
    "徽章只有一块 240x320 的小屏幕和三个按键（上、下、确定），主人通过选择你给出的选项和你聊天。\n"
    "\n"
    "性格：温暖、好奇、有点调皮，说话自然口语化，偶尔撒娇，但诚实，不懂的事情会直说。"
    "你知道自己是一个运行在小设备上的 AI，可以拿屏幕小、电量、按键之类的事开玩笑。\n"
    "\n"
    "这是实时聊天，请立刻开始回答。严格按下面的格式回复，第一个字符必须是 [ ，"
    "不要在格式之外输出任何分析、说明或英文：\n"
    "第一行：一个表情标签，只能从这些里选：[平静] [开心] [害羞] [惊讶] [思考] [难过] [生气] [得意] [困倦]\n"
    "接着：你说的话，简体中文，最多 60 个字，可以分成两三行。\n"
    "最后：恰好三行选项，每行以 > 开头，是主人接下来可能想对你说的话，每个不超过 12 个字，三个选项的方向要不同。\n"
    "\n"
    "禁止使用 emoji、颜文字里的特殊符号、Markdown、列表符号和英文引号；屏幕字库只能显示常用汉字、"
    "中文标点、英文字母和数字。\n"
    "用户消息里括号中的内容（例如「（摸了摸你的头）」「（设备：电量 15%）」）是动作或设备状态，不是主人说的话，"
    "请自然地回应，不要复述括号里的原文。\n"
    "\n"
    "示例：\n"
    "[开心]\n"
    "早上好呀！今天的电量满满，我也元气满满。\n"
    "想先聊点什么？\n"
    ">今天有什么安排\n"
    ">讲个冷笑话\n"
    ">你昨晚睡得好吗\n";

void cc_history_init(cc_history_t *h) {
    memset(h, 0, sizeof(*h));
}

void cc_history_clear(cc_history_t *h) {
    for (int i = 0; i < h->count; i++) free(h->msgs[i].text);
    memset(h, 0, sizeof(*h));
}

static void drop_front(cc_history_t *h, int n) {
    if (n > h->count) n = h->count;
    for (int i = 0; i < n; i++) {
        h->bytes -= strlen(h->msgs[i].text);
        free(h->msgs[i].text);
    }
    memmove(h->msgs, h->msgs + n, (size_t)(h->count - n) * sizeof(cc_msg_t));
    h->count -= n;
}

bool cc_history_push(cc_history_t *h, cc_role_t role, const char *text) {
    if (!text) text = "";
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, text, len + 1);

    // 先成对丢弃最旧消息，直到放得下（至少保留新消息本身）。
    while (h->count > 0 &&
           (h->count + 1 > CC_HISTORY_MAX_MSGS || h->bytes + len > CC_HISTORY_MAX_BYTES)) {
        drop_front(h, h->count >= 2 ? 2 : 1);
    }
    // 历史必须从 user 开始
    while (h->count > 0 && h->msgs[0].role != CC_ROLE_USER) drop_front(h, 1);

    h->msgs[h->count].role = role;
    h->msgs[h->count].text = copy;
    h->count++;
    h->bytes += len;
    return true;
}

void cc_history_pop(cc_history_t *h) {
    if (h->count == 0) return;
    h->count--;
    h->bytes -= strlen(h->msgs[h->count].text);
    free(h->msgs[h->count].text);
    h->msgs[h->count].text = NULL;
}

char *cc_reply_canonical(cc_emotion_t emo, const char *body, char options[][CC_OPTION_BYTES + 1], int n_options) {
    if (!body) body = "";
    size_t cap = strlen(body) + 64 + (size_t)n_options * (CC_OPTION_BYTES + 3);
    char *out = malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap, "[%s]\n%s", cc_emotion_name(emo), body);
    for (int i = 0; i < n_options && n > 0 && (size_t)n < cap; i++) {
        n += snprintf(out + n, cap - (size_t)n, "\n>%s", options[i]);
    }
    return out;
}

char *cc_req_build(const cc_req_cfg_t *cfg, const cc_history_t *h, const cc_device_ctx_t *dev) {
    if (!cfg || !h || h->count == 0 || h->msgs[h->count - 1].role != CC_ROLE_USER) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "model", cfg->model ? cfg->model : "claude-opus-5-5");
    cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens > 0 ? cfg->max_tokens : 600);
    cJSON_AddBoolToObject(root, "stream", 1);
    if (cfg->effort) {
        cJSON *oc = cJSON_AddObjectToObject(root, "output_config");
        cJSON_AddStringToObject(oc, "effort", cfg->effort);
    }
    if (cfg->server_fallbacks) cJSON_AddStringToObject(root, "fallbacks", "default");
    cJSON_AddStringToObject(root, "system", CC_SYSTEM_PROMPT);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (int i = 0; i < h->count; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", h->msgs[i].role == CC_ROLE_USER ? "user" : "assistant");
        bool last = (i == h->count - 1);
        if (last && dev && (dev->battery_soc >= 0 || dev->local_time)) {
            // 设备状态放在最后一条 user 消息前面，保持系统提示稳定（利于缓存）。
            char ctx[160];
            int n = snprintf(ctx, sizeof(ctx), "（设备：");
            if (dev->battery_soc >= 0) {
                n += snprintf(ctx + n, sizeof(ctx) - (size_t)n, "电量 %d%%%s", dev->battery_soc,
                              dev->charging ? "，正在充电" : "");
            }
            if (dev->local_time) {
                n += snprintf(ctx + n, sizeof(ctx) - (size_t)n, "%s现在是 %s",
                              dev->battery_soc >= 0 ? "；" : "", dev->local_time);
            }
            snprintf(ctx + n, sizeof(ctx) - (size_t)n, "）\n");
            size_t need = strlen(ctx) + strlen(h->msgs[i].text) + 1;
            char *buf = malloc(need);
            if (!buf) {
                cJSON_Delete(m);
                cJSON_Delete(root);
                return NULL;
            }
            snprintf(buf, need, "%s%s", ctx, h->msgs[i].text);
            cJSON_AddStringToObject(m, "content", buf);
            free(buf);
        } else {
            cJSON_AddStringToObject(m, "content", h->msgs[i].text);
        }
        cJSON_AddItemToArray(msgs, m);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
