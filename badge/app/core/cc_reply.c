// cc_reply.c — 见 cc_reply.h。
#include "cc_reply.h"

#include <string.h>

static const char *const EMO_NAMES[CC_EMO_COUNT] = {
    "平静", "开心", "害羞", "惊讶", "思考", "难过", "生气", "得意", "困倦",
};

// 全角括号与大于号的 UTF-8 编码
static const unsigned char FW_LBRACKET[3] = {0xE3, 0x80, 0x90};  // 【
static const unsigned char FW_RBRACKET[3] = {0xE3, 0x80, 0x91};  // 】
static const unsigned char FW_GT[3] = {0xEF, 0xBC, 0x9E};        // ＞

cc_emotion_t cc_emotion_from_name(const char *name) {
    if (!name) return CC_EMO_CALM;
    for (int i = 0; i < CC_EMO_COUNT; i++) {
        if (strcmp(name, EMO_NAMES[i]) == 0) return (cc_emotion_t)i;
    }
    return CC_EMO_CALM;
}

const char *cc_emotion_name(cc_emotion_t emo) {
    return (emo >= 0 && emo < CC_EMO_COUNT) ? EMO_NAMES[emo] : EMO_NAMES[0];
}

void cc_reply_init(cc_reply_t *r, const cc_reply_cbs_t *cbs) {
    memset(r, 0, sizeof(*r));
    if (cbs) r->cbs = *cbs;
    r->state = CC_RP_TAG;
    r->at_line_start = true;
}

// ---- 正文输出 -------------------------------------------------------------

// 返回 buf[0..len) 中最后一个完整 UTF-8 字符之后的位置。
static size_t utf8_complete_prefix(const char *buf, size_t len) {
    if (len == 0) return 0;
    size_t i = len;
    size_t back = 0;
    // 向前最多看 3 个续字节，找到最后一个起始字节
    while (i > 0 && back < 4) {
        unsigned char c = (unsigned char)buf[i - 1];
        if ((c & 0xC0) != 0x80) {
            size_t start = i - 1;
            size_t need = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
            return (len - start >= need) ? len : start;
        }
        i--;
        back++;
    }
    return len;  // 不合法序列：原样交出，避免卡死
}

static void flush_stage(cc_reply_t *r, bool final) {
    if (r->stage_len == 0) return;
    size_t cut = final ? r->stage_len : utf8_complete_prefix(r->stage, r->stage_len);
    if (cut > 0 && r->cbs.on_body) r->cbs.on_body(r->cbs.ctx, r->stage, cut);
    memmove(r->stage, r->stage + cut, r->stage_len - cut);
    r->stage_len -= cut;
}

static void stage_byte(cc_reply_t *r, char c) {
    if (r->stage_len >= sizeof(r->stage)) flush_stage(r, false);
    if (r->stage_len >= sizeof(r->stage)) flush_stage(r, true);  // 只剩残缺字符：强制交出
    r->stage[r->stage_len++] = c;
}

static void body_text_byte(cc_reply_t *r, char c) {
    if (r->body_started) {
        // 连续空行最多保留一个空行
        unsigned nl = r->pending_newlines > 2 ? 2 : r->pending_newlines;
        while (nl--) stage_byte(r, '\n');
    }
    r->pending_newlines = 0;
    r->body_started = true;
    stage_byte(r, c);
}

// ---- 选项 -----------------------------------------------------------------

static void finish_option(cc_reply_t *r) {
    char *s = r->opt;
    size_t n = r->opt_len;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) n--;
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    if (n > 0 && r->option_count < CC_MAX_OPTIONS) {
        char *dst = r->options[r->option_count];
        size_t keep = utf8_complete_prefix(s, n);  // 截断时不留半个字符
        memcpy(dst, s, keep);
        dst[keep] = '\0';
        if (r->cbs.on_option) r->cbs.on_option(r->cbs.ctx, r->option_count, dst);
        r->option_count++;
    }
    r->opt_len = 0;
    r->in_option = false;
    r->at_line_start = true;
}

static void start_option(cc_reply_t *r) {
    flush_stage(r, false);
    r->state = CC_RP_OPTIONS;
    r->in_option = true;
    r->opt_len = 0;
    r->head_len = 0;
    r->at_line_start = false;
}

// ---- 逐字节处理 -----------------------------------------------------------

static void body_byte(cc_reply_t *r, char c);

// 行首缓冲已经确定不是选项：作为正文（或选项区里的杂行）处理。
static void release_head_as_text(cc_reply_t *r) {
    char tmp[sizeof(r->head)];
    size_t n = r->head_len;
    memcpy(tmp, r->head, n);
    r->head_len = 0;
    r->at_line_start = false;
    if (r->state == CC_RP_OPTIONS) {
        r->skip_line = true;
        return;
    }
    for (size_t i = 0; i < n; i++) body_text_byte(r, tmp[i]);
}

static void body_byte(cc_reply_t *r, char c) {
    if (r->in_option) {
        if (c == '\n') {
            finish_option(r);
        } else if (r->opt_len < CC_OPTION_BYTES) {
            r->opt[r->opt_len++] = c;
        }
        return;
    }
    if (r->skip_line) {
        if (c == '\n') {
            r->skip_line = false;
            r->at_line_start = true;
        }
        return;
    }
    if (r->at_line_start) {
        if (r->head_len == 0) {
            if (c == ' ' || c == '\t' || c == '\r') return;  // 行首空白丢弃
            if (c == '\n') {
                if (r->body_started) r->pending_newlines++;
                return;
            }
        }
        r->head[r->head_len++] = c;
        if (r->head[0] == '>') {
            start_option(r);
            return;
        }
        // 可能是全角 ＞ 的前缀
        size_t m = r->head_len < 3 ? r->head_len : 3;
        if (memcmp(r->head, FW_GT, m) == 0) {
            if (r->head_len >= 3) start_option(r);
            return;  // 还不够 3 字节：继续等
        }
        release_head_as_text(r);
        return;
    }
    if (c == '\n') {
        r->at_line_start = true;
        if (r->body_started) r->pending_newlines++;
        return;
    }
    if (c == '\r') return;
    body_text_byte(r, c);
}

// 标签区判定失败：表情记为平静，把攒下的所有字节当正文重放。
static void tag_fail(cc_reply_t *r) {
    char tmp[CC_PREAMBLE_BYTES + 1];
    size_t n = r->pre_len;
    memcpy(tmp, r->pre, n);
    r->pre_len = 0;
    r->line_start = 0;
    r->state = CC_RP_BODY;
    r->emotion = CC_EMO_CALM;
    if (r->cbs.on_emotion) r->cbs.on_emotion(r->cbs.ctx, CC_EMO_CALM);
    for (size_t i = 0; i < n; i++) body_byte(r, tmp[i]);
}

static bool is_known_emotion(const char *name) {
    for (int i = 0; i < CC_EMO_COUNT; i++) {
        if (strcmp(name, EMO_NAMES[i]) == 0) return true;
    }
    return false;
}

// 尝试把 pre[from..pre_len) 当作「[名字]」或「【名字】」标签。
// 第一行接受任意名字（未知按平静）；后面的行只接受已知表情，免得把正文里的方括号当标签。
// 成功时丢弃标签前的杂行，进入正文状态。
static bool try_close_tag(cc_reply_t *r, size_t from, size_t open_len, size_t close_len) {
    char name[CC_PREAMBLE_BYTES + 1];
    size_t start = from + open_len, end = r->pre_len - close_len;
    while (start < end && r->pre[start] == ' ') start++;
    while (end > start && r->pre[end - 1] == ' ') end--;
    if (end - start > 24) return false;  // 表情名不会这么长
    memcpy(name, r->pre + start, end - start);
    name[end - start] = '\0';
    bool first_line = (r->pre_lines == 0);
    if (!first_line && !is_known_emotion(name)) return false;
    r->emotion = cc_emotion_from_name(name);
    r->tagged = true;
    r->pre_len = 0;
    r->line_start = 0;
    r->state = CC_RP_BODY;
    r->at_line_start = true;
    if (r->cbs.on_emotion) r->cbs.on_emotion(r->cbs.ctx, r->emotion);
    return true;
}

static void tag_byte(cc_reply_t *r, char c) {
    if (r->pre_len == 0 && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) return;
    if (r->pre_len >= CC_PREAMBLE_BYTES) {
        tag_fail(r);
        body_byte(r, c);
        return;
    }
    r->pre[r->pre_len++] = c;

    if (c == '\n') {
        r->pre_lines++;
        r->line_start = r->pre_len;
        if (r->pre_lines >= CC_PREAMBLE_LINES) tag_fail(r);
        return;
    }

    // 当前行的第一个非空白字符
    size_t s = r->line_start;
    while (s < r->pre_len && (r->pre[s] == ' ' || r->pre[s] == '\t' || r->pre[s] == '\r')) s++;
    if (s >= r->pre_len) return;
    const unsigned char *t = (const unsigned char *)r->pre + s;
    size_t avail = r->pre_len - s;
    if (t[0] == '[') {
        if (c == ']') try_close_tag(r, s, 1, 1);
        return;
    }
    size_t m = avail < 3 ? avail : 3;
    if (memcmp(t, FW_LBRACKET, m) == 0 && avail >= 6 &&
        memcmp(r->pre + r->pre_len - 3, FW_RBRACKET, 3) == 0) {
        try_close_tag(r, s, 3, 3);
    }
    // 其他情况：这一行是杂行（或没有标签的正文），继续攒到行尾再判断。
}

void cc_reply_feed(cc_reply_t *r, const char *utf8, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (r->state == CC_RP_TAG) {
            tag_byte(r, utf8[i]);
        } else {
            body_byte(r, utf8[i]);
        }
    }
    flush_stage(r, false);
}

void cc_reply_finish(cc_reply_t *r) {
    if (r->state == CC_RP_TAG) {
        if (r->pre_len > 0) {
            tag_fail(r);
        } else {
            r->state = CC_RP_BODY;
            if (r->cbs.on_emotion) r->cbs.on_emotion(r->cbs.ctx, CC_EMO_CALM);
        }
    }
    if (r->in_option) {
        finish_option(r);
    } else if (r->at_line_start && r->head_len > 0) {
        release_head_as_text(r);
    }
    flush_stage(r, true);
}

bool cc_reply_looks_valid(const cc_reply_t *r, size_t body_bytes) {
    return r->option_count >= 1 && body_bytes >= 6;  // 至少两个汉字
}
