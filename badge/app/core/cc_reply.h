// cc_reply.h — Claude娘 回复格式的增量解析器。
//
// 约定的模型输出格式（系统提示里规定）：
//
//   [开心]
//   正文，可以有多行。
//   >选项一
//   >选项二
//   >选项三
//
// 文本按任意字节边界分块到达（可能把一个 UTF-8 字符切开），解析器负责：
//   - 识别开头的 [表情] 标签（没有标签时按「平静」处理，正文照常显示）；
//   - 把正文逐块交给 UI（不会把半个 UTF-8 字符交出去）；
//   - 以 '>' 或全角 '＞' 开头的行作为选项，最多 CC_MAX_OPTIONS 个。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CC_EMO_CALM = 0,   // 平静
    CC_EMO_HAPPY,      // 开心
    CC_EMO_SHY,        // 害羞
    CC_EMO_SURPRISED,  // 惊讶
    CC_EMO_THINKING,   // 思考
    CC_EMO_SAD,        // 难过
    CC_EMO_ANGRY,      // 生气
    CC_EMO_PROUD,      // 得意
    CC_EMO_SLEEPY,     // 困倦
    CC_EMO_COUNT
} cc_emotion_t;

#define CC_MAX_OPTIONS   3
#define CC_OPTION_BYTES  96
// 标签前允许出现的杂行（模型偶尔先漏出一句分析）：最多这么多字节 / 行，
// 在此范围内出现合法表情标签就丢掉前面的杂行；超出则全部当正文显示。
#define CC_PREAMBLE_BYTES 256
#define CC_PREAMBLE_LINES 3

typedef struct {
    void (*on_emotion)(void *ctx, cc_emotion_t emo);
    void (*on_body)(void *ctx, const char *utf8, size_t len);
    void (*on_option)(void *ctx, int index, const char *utf8);
    void *ctx;
} cc_reply_cbs_t;

typedef enum {
    CC_RP_TAG = 0,   // 还在等开头的 [表情]
    CC_RP_BODY,      // 正文
    CC_RP_OPTIONS,   // 已进入选项区，之后的非选项行忽略
} cc_reply_state_t;

typedef struct {
    cc_reply_cbs_t cbs;
    cc_reply_state_t state;

    char pre[CC_PREAMBLE_BYTES + 1];  // TAG 状态下收到的全部字节
    size_t pre_len;
    size_t line_start;                // 当前行在 pre 中的起点
    int pre_lines;                    // 已经完整收到的行数

    // 行首缓冲：判断一行是不是选项之前先攒住（最多 3 字节就能判断）。
    char head[8];
    size_t head_len;
    bool at_line_start;
    bool in_option;
    bool body_started;
    unsigned pending_newlines;  // 正文中延迟输出的换行（避免把选项前的空行显示出来）

    bool skip_line;             // 选项区里出现的非选项行：整行忽略

    char opt[CC_OPTION_BYTES + 1];
    size_t opt_len;
    int option_count;

    // 正文暂存：攒够或一次 feed 结束时按 UTF-8 字符边界交给 UI，
    // 末尾不完整的字符留到下次。
    char stage[128];
    size_t stage_len;

    cc_emotion_t emotion;
    bool tagged;                // 见到了真正的表情标签（而不是缺省的平静）
    char options[CC_MAX_OPTIONS][CC_OPTION_BYTES + 1];
} cc_reply_t;

void cc_reply_init(cc_reply_t *r, const cc_reply_cbs_t *cbs);
void cc_reply_feed(cc_reply_t *r, const char *utf8, size_t len);
void cc_reply_finish(cc_reply_t *r);

// "开心" -> CC_EMO_HAPPY；未知返回 CC_EMO_CALM。
cc_emotion_t cc_emotion_from_name(const char *name);
const char *cc_emotion_name(cc_emotion_t emo);

// 回复是否像样：有正文、至少一个选项。模型偶尔会输出退化内容（几个乱码字符），
// 应用据此决定是否静默重试一次。body_bytes 是应用累计收到的正文字节数。
bool cc_reply_looks_valid(const cc_reply_t *r, size_t body_bytes);

#ifdef __cplusplus
}
#endif
