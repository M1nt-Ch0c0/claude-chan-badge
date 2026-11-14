// claude_chan_app.c — Claude娘：视觉小说式的 AI 陪伴应用。
//
// 交互：确定单击 = 暂停或继续语音。确定双击 = 摸摸头。
// 文字对话不在屏幕上，语音走局域网，由 Mac 上的桥完成。
//
// 所有回调（按键、定时器、HTTP）都在 LVGL 线程上执行。
#include "claude_chan_app.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "cc_avatar.h"
#include "cc_chat.h"
#include "cc_platform.h"
#include "cc_reply.h"
#include "cc_sse.h"
#include "cc_stream.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "claude_chan";

#define C_TEXT      0x3D2B2B
#define C_SUBTLE    0x9A8478
#define C_ACCENT    0xE0784A
#define C_ROW       0xFFF1E6
#define C_USER      0x5560C8
#define SUB_CAP     256
#define SUB_LINES   3
#define SUB_SPACE   1
#define SUB_BOTTOM  284   // 字幕框下沿
#define SUBIMG_W    200
#define SUBIMG_HMAX 72
#define BODY_CAP    1024
#define ERR_CAP     384
#define IDLE_SLEEP_MS (60 * 1000)
#define MAX_ROWS    3

typedef enum {
    ST_IDLE = 0,     // 没有对话进行（刚进入或出错后休息）
    ST_WAITING,      // 请求已发出，还没收到正文
    ST_STREAMING,    // 正在接收 / 打字机输出
    ST_TYPED,        // 说完了，等确定键打开选项
    ST_CHOOSING,     // 选项界面
    ST_ERROR,        // 出错，选项为「再试一次 / 先休息一下」
    ST_SLEEPING,     // 打瞌睡（本地状态，不请求网络）
} state_t;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *time_lbl;
    lv_obj_t *batt_lbl;
    lv_obj_t *batt_fill;
    lv_obj_t *net_lbl;
    lv_obj_t *box;
    lv_obj_t *body;
    lv_obj_t *more;
    lv_obj_t *hint;
    lv_obj_t *sub_box;   // 语音字幕：Mac 发来的最新一段
    lv_obj_t *sub_clip;  // 高度正好是整数行，裁掉的总是完整的旧行
    lv_obj_t *sub_lbl;
    lv_obj_t *sub_img;   // Mac 渲染的字幕位图（有了就不用 sub_lbl）
    uint8_t *sub_a8;
    lv_image_dsc_t sub_dsc;
    unsigned img_ver;
    unsigned sub_ver;
    unsigned hold_ver;
    bool voice_hold;
    lv_obj_t *row[MAX_ROWS];
    lv_obj_t *row_lbl[MAX_ROWS];
    cc_avatar_t *avatar;

    lv_timer_t *tick_timer;
    lv_timer_t *type_timer;
    lv_timer_t *dots_timer;
    lv_timer_t *phase_timer;
    int shown_phase;

    state_t st;
    cc_history_t hist;
    cc_sse_t *sse;  // 约 4 KB，按需分配
    cc_stream_t stream;
    cc_reply_t reply;

    char *full;      // 已收到的正文
    size_t full_len;
    size_t shown;    // 打字机已显示到的字节位置
    bool reply_done;

    char opts[MAX_ROWS][CC_OPTION_BYTES + 1];
    int n_opts;
    int sel;

    unsigned gen;     // 请求代号：旧请求的回调一律丢弃
    int req_id;
    bool retried;
    char *pending_user;  // 正在等待回复的那句（失败时用于重试）
    char err[ERR_CAP];
    size_t err_len;
    int dots;

    int last_soc;
    bool last_charging;
    bool low_batt_notified;
    uint32_t idle_ms;
    bool offline;
} app_t;

static app_t *s;

// ---- 小工具 ---------------------------------------------------------------

static size_t utf8_len(unsigned char c) {
    return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
}

static char *dup_str(const char *t) {
    size_t n = strlen(t);
    char *p = cc_platform_alloc(n + 1);
    if (p) memcpy(p, t, n + 1);
    return p;
}

static void set_body_text(const char *text, uint32_t color) {
    lv_obj_set_style_text_color(s->body, lv_color_hex(color), 0);
    lv_label_set_text(s->body, text);
    lv_obj_scroll_to_y(s->box, 0, LV_ANIM_OFF);
}

// 正文超出对话框时，滚到底部让最新一行可见。
static void keep_body_bottom_visible(void) {
    lv_obj_update_layout(s->box);
    int32_t over = lv_obj_get_height(s->body) + 14 - lv_obj_get_content_height(s->box);
    lv_obj_scroll_to_y(s->box, over > 0 ? over : 0, LV_ANIM_OFF);
}

// 对话框右下角的指示器：说完时显示「▼」，等待回复时显示跳动的点。
static void update_more(state_t st) {
    if (st == ST_TYPED) {
        lv_obj_set_style_text_font(s->more, LV_FONT_DEFAULT, 0);  // 中文子集里没有 ▼，用内置符号
        lv_label_set_text(s->more, LV_SYMBOL_DOWN);
        lv_obj_remove_flag(s->more, LV_OBJ_FLAG_HIDDEN);
    } else if (st == ST_WAITING) {
        lv_obj_set_style_text_font(s->more, cc_platform_font(14), 0);
        lv_label_set_text(s->more, "·");
        lv_obj_remove_flag(s->more, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s->more, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_rows(void) {
    bool choosing = (s->st == ST_CHOOSING || s->st == ST_ERROR);
    if (choosing) {
        lv_obj_add_flag(s->body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(s->box, 0, LV_ANIM_OFF);
    } else {
        lv_obj_remove_flag(s->body, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < MAX_ROWS; i++) {
        bool vis = choosing && i < s->n_opts;
        if (!vis) {
            lv_obj_add_flag(s->row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s->row[i], LV_OBJ_FLAG_HIDDEN);
        bool on = (i == s->sel);
        lv_obj_set_style_bg_color(s->row[i], lv_color_hex(on ? C_ACCENT : C_ROW), 0);
        lv_obj_set_style_text_color(s->row_lbl[i], lv_color_hex(on ? 0xFFFFFF : C_TEXT), 0);
        lv_label_set_text(s->row_lbl[i], s->opts[i]);
    }
}

static void set_state(state_t st) {
    s->st = st;
    if (st != ST_CHOOSING && st != ST_TYPED) s->idle_ms = 0;
    update_more(st);
    render_rows();
}

static void set_local_options(const char *a, const char *b, const char *c) {
    const char *src[MAX_ROWS] = {a, b, c};
    s->n_opts = 0;
    for (int i = 0; i < MAX_ROWS && src[i]; i++) {
        snprintf(s->opts[i], sizeof(s->opts[i]), "%s", src[i]);
        s->n_opts++;
    }
    s->sel = 0;
}

// 本地说一句话（不经网络），然后进入指定状态。
static void say_local(cc_emotion_t emo, const char *text, state_t next) {
    cc_avatar_set_emotion(s->avatar, emo);
    cc_avatar_set_talking(s->avatar, false);
    set_body_text(text, C_TEXT);
    set_state(next);
}

// ---- 状态栏 ---------------------------------------------------------------

static void format_time(char *buf, size_t cap, bool with_weekday) {
    static const char *const WD[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (with_weekday) {
        snprintf(buf, cap, "%s %02d:%02d", WD[tm.tm_wday], tm.tm_hour, tm.tm_min);
    } else {
        snprintf(buf, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
}

static void update_status_bar(void) {
    char buf[32];
    format_time(buf, sizeof(buf), false);
    lv_label_set_text(s->time_lbl, buf);

    int soc = cc_platform_battery_soc();
    bool chg = cc_platform_charging();
    if (soc < 0) {
        lv_label_set_text(s->batt_lbl, "--%");
        soc = 0;
    } else {
        snprintf(buf, sizeof(buf), chg ? "充电 %d%%" : "%d%%", soc);
        lv_label_set_text(s->batt_lbl, buf);
    }
    lv_obj_set_width(s->batt_fill, 1 + (17 * soc) / 100);
    uint32_t col = chg ? 0x5BBF6A : soc <= 20 ? 0xE5484D : soc <= 50 ? 0xF2A33A : 0x5BBF6A;
    lv_obj_set_style_bg_color(s->batt_fill, lv_color_hex(col), 0);
    if (s->offline) {
        lv_obj_remove_flag(s->net_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s->net_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- 网络请求 -------------------------------------------------------------

static void on_http_body(void *user, const uint8_t *data, size_t len);
static void on_http_done(void *user, int status, int err);

static void reset_reply_parsers(void);

static void start_request(void) {
    s->gen++;
    reset_reply_parsers();
    s->full_len = 0;
    s->full[0] = '\0';
    s->shown = 0;
    s->reply_done = false;
    s->err_len = 0;
    s->n_opts = 0;
    s->sel = 0;

    char when[32];
    format_time(when, sizeof(when), true);
    const char *model = cc_platform_model();
    // 只有真正的 Claude 模型才带 effort / fallbacks，别家模型带上会 400。
    // 有的中转把别家模型挂成 claude-xxx-dd-<名字> 的别名，这种也不算。
    bool claude = strncmp(model, "claude-", 7) == 0 && strstr(model, "-dd-") == NULL;
    cc_req_cfg_t cfg = {model, 600, claude ? "low" : NULL, claude};
    cc_device_ctx_t dev = {cc_platform_battery_soc(), cc_platform_charging(), when};
    char *json = cc_req_build(&cfg, &s->hist, &dev);
    if (!json) {
        ESP_LOGE(TAG, "构建请求失败");
        say_local(CC_EMO_SAD, "唔……我脑袋里有点乱，等一下再试好吗？", ST_ERROR);
        set_local_options("再试一次", "先休息一下", NULL);
        render_rows();
        return;
    }
    s->req_id = cc_platform_http_post(CC_REQ_PATH, CC_REQ_HEADERS, json, strlen(json), on_http_body, on_http_done,
                                      (void *)(uintptr_t)s->gen);
    ESP_LOGI(TAG, "请求已发出 gen=%u id=%d 历史=%d 条 %u 字节 模型=%s", s->gen, s->req_id, s->hist.count,
             (unsigned)s->hist.bytes, cfg.model);
    cJSON_free(json);
    if (s->req_id < 0) {
        on_http_done((void *)(uintptr_t)s->gen, 0, -1);
        return;
    }
    cc_avatar_set_emotion(s->avatar, CC_EMO_THINKING);
    s->dots = 0;
    set_state(ST_WAITING);
}

// 把一句话（选项文字或括号里的动作）发给 Claude娘。
static void send_user(const char *text) {
    if (s->st == ST_WAITING || s->st == ST_STREAMING) return;
    if (!cc_history_push(&s->hist, CC_ROLE_USER, text)) {
        ESP_LOGE(TAG, "内存不足，无法记录对话");
        return;
    }
    cc_platform_free(s->pending_user);
    s->pending_user = dup_str(text);
    s->retried = false;
    // 等待期间在对话框里显示主人说的话
    char shown[CC_OPTION_BYTES + 64];
    if (strncmp(text, "（", strlen("（")) == 0) {
        snprintf(shown, sizeof(shown), "%s", text);
    } else {
        snprintf(shown, sizeof(shown), "你：「%s」", text);
    }
    set_body_text(shown, C_SUBTLE);
    start_request();
}

static void fail_request(const char *reason_for_log, const char *text) {
    ESP_LOGW(TAG, "请求失败：%s", reason_for_log);
    cc_history_pop(&s->hist);  // 撤回这句，重试时重新加
    cc_avatar_set_talking(s->avatar, false);
    say_local(CC_EMO_SAD, text, ST_ERROR);
    set_local_options("再试一次", "先休息一下", NULL);
    render_rows();
}

static void finish_success(void) {
    char *canon = cc_reply_canonical(s->reply.emotion, s->full, s->opts, s->n_opts);
    if (canon) {
        cc_history_push(&s->hist, CC_ROLE_ASSISTANT, canon);
        free(canon);  // cc_reply_canonical 用 malloc 分配
    }
    if (s->n_opts == 0) {
        set_local_options("嗯嗯", "然后呢？", "换个话题吧");
    }
    s->offline = false;
    ESP_LOGI(TAG, "回复完成 表情=%s 正文=%u 字节 选项=%d 停止=%s 模型=%s 输出=%d", cc_emotion_name(s->reply.emotion),
             (unsigned)s->full_len, s->n_opts, s->stream.stop_reason, s->stream.model, s->stream.output_tokens);
}

// ---- 解析回调 -------------------------------------------------------------

static void rp_emotion(void *ctx, cc_emotion_t emo) {
    (void)ctx;
    cc_avatar_set_emotion(s->avatar, emo);
}

static void rp_body(void *ctx, const char *t, size_t n) {
    (void)ctx;
    if (s->full_len + n >= BODY_CAP) n = BODY_CAP - 1 - s->full_len;
    memcpy(s->full + s->full_len, t, n);
    s->full_len += n;
    s->full[s->full_len] = '\0';
}

static void rp_option(void *ctx, int i, const char *t) {
    (void)ctx;
    if (i >= MAX_ROWS) return;
    snprintf(s->opts[i], sizeof(s->opts[i]), "%s", t);
    if (i + 1 > s->n_opts) s->n_opts = i + 1;
}

static void st_text(void *ctx, const char *t, size_t n) {
    (void)ctx;
    cc_reply_feed(&s->reply, t, n);
}

static void st_done(void *ctx, const char *stop) {
    (void)ctx;
    (void)stop;
}

static void st_error(void *ctx, const char *type, const char *msg) {
    (void)ctx;
    ESP_LOGW(TAG, "流错误 %s: %s", type, msg);
}

static void reset_reply_parsers(void) {
    cc_reply_cbs_t rc = {rp_emotion, rp_body, rp_option, NULL};
    cc_reply_init(&s->reply, &rc);
    cc_stream_cbs_t sc = {st_text, st_done, st_error, NULL};
    cc_stream_init(&s->stream, &sc);
    cc_sse_init(s->sse, cc_stream_sse_cb, &s->stream);
}

static void on_http_body(void *user, const uint8_t *data, size_t len) {
    if (!s || (uintptr_t)user != s->gen) return;
    // 留一份原始字节的开头，非 2xx 时用来显示错误
    if (s->err_len < ERR_CAP - 1) {
        size_t n = len < ERR_CAP - 1 - s->err_len ? len : ERR_CAP - 1 - s->err_len;
        memcpy(s->err + s->err_len, data, n);
        s->err_len += n;
        s->err[s->err_len] = '\0';
    }
    cc_sse_feed(s->sse, (const char *)data, len);
    if (s->st == ST_WAITING && (s->reply.tagged || s->full_len >= 24)) {
        set_body_text("", C_TEXT);
        set_state(ST_STREAMING);
    }
}

static void on_http_done(void *user, int status, int err) {
    if (!s || (uintptr_t)user != s->gen) return;
    s->req_id = -1;
    cc_sse_finish(s->sse);
    if (err != 0 || status < 200 || status >= 300) {
        char reason[96] = "";
        cJSON *j = s->err_len ? cJSON_Parse(s->err) : NULL;
        const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
        const cJSON *t = e ? cJSON_GetObjectItemCaseSensitive(e, "type") : NULL;
        snprintf(reason, sizeof(reason), "status=%d err=%d %s", status, err, cJSON_IsString(t) ? t->valuestring : "");
        cJSON_Delete(j);
        if (status == 0) s->offline = true;
        update_status_bar();
        fail_request(reason, status == 0 ? "连不上网络了……\n你看看 Wi-Fi 是不是关掉了？"
                                         : "服务器那边好像出了点状况，\n我们等一下再聊吧。");
        return;
    }
    if (s->stream.errored) {
        fail_request("stream error event", "说到一半信号断掉了……\n再说一遍好吗？");
        return;
    }
    cc_reply_finish(&s->reply);
    if (strcmp(s->stream.stop_reason, "refusal") == 0) {
        cc_history_pop(&s->hist);
        s->gen++;  // 丢弃这次的残余输出
        say_local(CC_EMO_THINKING, "这个话题我不太方便聊呢，\n我们换一个好不好？", ST_TYPED);
        set_local_options("好吧，换一个", "讲个笑话吧", "你今天开心吗");
        return;
    }
    if (!cc_reply_looks_valid(&s->reply, s->full_len)) {
        ESP_LOGW(TAG, "回复不完整（正文 %u 字节，选项 %d 个），%s", (unsigned)s->full_len, s->reply.option_count,
                 s->retried ? "放弃" : "静默重试");
        if (!s->retried) {
            s->retried = true;
            start_request();  // 历史里仍是同一句 user
            return;
        }
        cc_history_pop(&s->hist);
        say_local(CC_EMO_SAD, "唔……我脑袋卡住了，\n刚才说到哪儿了？", ST_TYPED);
        set_local_options("再说一次吧", "换个话题", "你还好吗");
        return;
    }
    s->reply_done = true;
    finish_success();
    if (s->st == ST_WAITING) {
        set_body_text("", C_TEXT);
        set_state(ST_STREAMING);
    }
}

// ---- 定时器 ---------------------------------------------------------------

static void type_cb(lv_timer_t *t) {
    (void)t;
    if (s->st != ST_STREAMING) return;
    if (s->shown < s->full_len) {
        size_t backlog = s->full_len - s->shown;
        int steps = backlog > 60 ? 3 : backlog > 24 ? 2 : 1;
        while (steps-- > 0 && s->shown < s->full_len) {
            s->shown += utf8_len((unsigned char)s->full[s->shown]);
            if (s->shown > s->full_len) s->shown = s->full_len;
        }
        char saved = s->full[s->shown];
        s->full[s->shown] = '\0';
        lv_label_set_text(s->body, s->full);
        s->full[s->shown] = saved;
        keep_body_bottom_visible();
        cc_avatar_set_talking(s->avatar, true);
        return;
    }
    cc_avatar_set_talking(s->avatar, false);
    if (s->reply_done) set_state(ST_TYPED);
}

static void apply_phase(int phase) {
    if (!s || !s->avatar || s->voice_hold) return;
    switch (phase) {
    case 1:
        cc_avatar_set_listening(s->avatar, true);
        lv_label_set_text(s->hint, "在听");
        break;
    case 2:
        cc_avatar_set_listening(s->avatar, false);
        cc_avatar_set_talking(s->avatar, false);
        cc_avatar_set_emotion(s->avatar, CC_EMO_THINKING);
        lv_label_set_text(s->hint, "在想");
        break;
    case 3:
        cc_avatar_set_emotion(s->avatar, CC_EMO_CALM);
        cc_avatar_set_talking(s->avatar, true);
        lv_label_set_text(s->hint, "在说");
        break;
    default:
        cc_avatar_set_listening(s->avatar, false);
        cc_avatar_set_talking(s->avatar, false);
        cc_avatar_set_emotion(s->avatar, CC_EMO_CALM);
        lv_label_set_text(s->hint, "按确定暂停");
        break;
    }
}

static void apply_hold(bool hold) {
    s->voice_hold = hold;
    s->shown_phase = -1;
    if (hold) {
        cc_avatar_set_listening(s->avatar, false);
        cc_avatar_set_talking(s->avatar, false);
        cc_avatar_set_emotion(s->avatar, CC_EMO_SLEEPY);
        lv_label_set_text(s->hint, "已暂停");
        lv_obj_add_flag(s->sub_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        cc_avatar_set_emotion(s->avatar, CC_EMO_CALM);
        lv_label_set_text(s->hint, "按确定暂停");
    }
}

// 字幕框只显示最后几行：文字比框高时贴底，旧的行从上沿裁掉。
static void show_subtitle(int role, const char *text) {
    if (role == 0 || !text[0] || s->voice_hold) {
        lv_obj_add_flag(s->sub_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const lv_font_t *font = cc_platform_font(role == 3 ? 14 : 16);
    uint32_t color = role == 1 ? C_USER : role == 3 ? C_SUBTLE : C_TEXT;
    lv_obj_set_style_text_font(s->sub_lbl, font, 0);
    lv_obj_set_style_text_color(s->sub_lbl, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(s->sub_box, lv_color_hex(role == 1 ? C_USER : C_ACCENT), 0);
    int32_t lh = lv_font_get_line_height(font);
    int32_t clip_h = SUB_LINES * lh + (SUB_LINES - 1) * SUB_SPACE;
    lv_obj_set_size(s->sub_clip, 202, clip_h);
    lv_obj_set_size(s->sub_box, 216, clip_h + 10);
    lv_obj_set_y(s->sub_box, SUB_BOTTOM - (clip_h + 10));
    lv_obj_center(s->sub_clip);
    lv_label_set_text(s->sub_lbl, text);
    lv_obj_remove_flag(s->sub_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s->sub_lbl);
    int32_t h = lv_obj_get_height(s->sub_lbl);
    lv_obj_align(s->sub_lbl, h > clip_h ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_CENTER, 0, 0);
}

// 取 Mac 发来的字幕位图。返回 true 表示这台 Mac 走位图通路（以后不再看文字）。
static bool poll_subimg(void) {
    if (!s->sub_a8) {
        s->sub_a8 = cc_platform_alloc(SUBIMG_W * SUBIMG_HMAX);
        if (!s->sub_a8) return false;
    }
    int w = 0, h = 0, role = 0;
    unsigned v = cc_platform_voice_subimg(s->sub_a8, SUBIMG_W * SUBIMG_HMAX, &w, &h, &role);
    if (v == 0) return s->img_ver != 0;
    if (v == s->img_ver) return true;
    s->img_ver = v;
    lv_obj_add_flag(s->sub_lbl, LV_OBJ_FLAG_HIDDEN);
    if (role == 0 || h == 0 || w == 0) {
        lv_obj_add_flag(s->sub_box, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    uint32_t color = role == 1 ? C_USER : role == 3 ? C_SUBTLE : C_TEXT;
    s->sub_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s->sub_dsc.header.cf = LV_COLOR_FORMAT_A8;
    s->sub_dsc.header.w = (uint32_t)w;
    s->sub_dsc.header.h = (uint32_t)h;
    s->sub_dsc.header.stride = (uint32_t)w;
    s->sub_dsc.data = s->sub_a8;
    s->sub_dsc.data_size = (uint32_t)(w * h);
    lv_image_set_src(s->sub_img, NULL);
    lv_image_set_src(s->sub_img, &s->sub_dsc);
    lv_obj_set_style_image_recolor(s->sub_img, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(s->sub_img, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s->sub_box, lv_color_hex(role == 1 ? C_USER : C_ACCENT), 0);
    lv_obj_set_size(s->sub_clip, w, h);
    lv_obj_set_size(s->sub_box, 216, h + 10);
    lv_obj_set_y(s->sub_box, SUB_BOTTOM - (h + 10));
    lv_obj_center(s->sub_clip);
    lv_obj_align(s->sub_img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(s->sub_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s->sub_box);
    return true;
}

static void phase_cb(lv_timer_t *t) {
    (void)t;
    if (!s) return;
    bool hold;
    unsigned hv = cc_platform_voice_remote_hold(&hold);
    if (hv != s->hold_ver) {
        s->hold_ver = hv;
        if (hold != s->voice_hold) apply_hold(hold);
    }
    if (s->voice_hold) return;
    if (poll_subimg()) goto phase;
    static char sub[SUB_CAP];
    int role = 0;
    unsigned sv = cc_platform_voice_text(sub, sizeof(sub), &role);
    if (sv != s->sub_ver) {
        s->sub_ver = sv;
        show_subtitle(role, sub);
    }
phase:;
    int phase = cc_platform_voice_phase();
    if (phase == s->shown_phase) return;
    s->shown_phase = phase;
    apply_phase(phase);
}

static void dots_cb(lv_timer_t *t) {
    (void)t;
    if (s->st != ST_WAITING) return;
    static const char *const FRAMES[3] = {"·", "··", "···"};
    s->dots = (s->dots + 1) % 3;
    lv_label_set_text(s->more, FRAMES[s->dots]);
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    update_status_bar();

    int soc = cc_platform_battery_soc();
    bool chg = cc_platform_charging();
    bool idle = (s->st == ST_CHOOSING || s->st == ST_TYPED || s->st == ST_IDLE || s->st == ST_SLEEPING);

    if (idle && chg && !s->last_charging) {
        cc_avatar_pat(s->avatar);
    } else if (idle && soc >= 0 && soc <= 20 && !chg && !s->low_batt_notified) {
        s->low_batt_notified = true;
    }
    if (soc > 25) s->low_batt_notified = false;  // 只有电量真正回升才允许再次提醒
    s->last_charging = chg;
    s->last_soc = soc;

    if (s->st == ST_CHOOSING || s->st == ST_TYPED) {
        s->idle_ms += 1000;
        if (s->idle_ms >= IDLE_SLEEP_MS) {
            ESP_LOGI(TAG, "太久没人理，打瞌睡了");
            say_local(CC_EMO_SLEEPY, "（Claude娘 抱着膝盖打起了瞌睡……）", ST_SLEEPING);
        }
    }
}

// ---- UI 构建 --------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void build_ui(void) {
    const lv_font_t *f16 = cc_platform_font(16);
    const lv_font_t *f14 = cc_platform_font(14);

    s->scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s->scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s->scr, lv_color_hex(0xFFF5EA), 0);
    lv_obj_set_style_bg_grad_color(s->scr, lv_color_hex(0xFFDCC8), 0);
    lv_obj_set_style_bg_grad_dir(s->scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s->scr, LV_OPA_COVER, 0);

    // 状态栏（避开 30 px 圆角）
    s->time_lbl = lv_label_create(s->scr);
    lv_obj_set_style_text_font(s->time_lbl, f14, 0);
    lv_obj_set_style_text_color(s->time_lbl, lv_color_hex(0x8C6A5A), 0);
    lv_obj_set_pos(s->time_lbl, 30, 9);

    s->net_lbl = lv_label_create(s->scr);
    lv_obj_set_style_text_font(s->net_lbl, f14, 0);
    lv_obj_set_style_text_color(s->net_lbl, lv_color_hex(0xE5484D), 0);
    lv_label_set_text(s->net_lbl, "离线");
    lv_obj_set_pos(s->net_lbl, 76, 9);
    lv_obj_add_flag(s->net_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *batt = plain(s->scr);
    lv_obj_set_pos(batt, 182, 12);
    lv_obj_set_size(batt, 21, 11);
    lv_obj_set_style_border_width(batt, 1, 0);
    lv_obj_set_style_border_color(batt, lv_color_hex(0x8C6A5A), 0);
    lv_obj_set_style_radius(batt, 3, 0);
    s->batt_fill = plain(batt);
    lv_obj_set_pos(s->batt_fill, 1, 1);
    lv_obj_set_size(s->batt_fill, 17, 7);
    lv_obj_set_style_radius(s->batt_fill, 2, 0);
    lv_obj_set_style_bg_opa(s->batt_fill, LV_OPA_COVER, 0);
    lv_obj_t *nub = plain(s->scr);
    lv_obj_set_pos(nub, 203, 15);
    lv_obj_set_size(nub, 2, 5);
    lv_obj_set_style_bg_color(nub, lv_color_hex(0x8C6A5A), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);

    s->batt_lbl = lv_label_create(s->scr);
    lv_obj_set_style_text_font(s->batt_lbl, f14, 0);
    lv_obj_set_style_text_color(s->batt_lbl, lv_color_hex(0x8C6A5A), 0);
    lv_obj_align(s->batt_lbl, LV_ALIGN_TOP_RIGHT, -62, 9);

    // 立绘
    s->avatar = cc_avatar_create(s->scr, (240 - CC_AVATAR_W) / 2, 40);

    // 语音字幕（压在立绘下半身上，像视觉小说的对话框）
    s->sub_box = plain(s->scr);
    lv_obj_set_pos(s->sub_box, 12, 206);
    lv_obj_set_size(s->sub_box, 216, 80);
    lv_obj_set_style_radius(s->sub_box, 14, 0);
    lv_obj_set_style_bg_color(s->sub_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s->sub_box, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s->sub_box, 1, 0);
    lv_obj_set_style_border_opa(s->sub_box, LV_OPA_50, 0);
    lv_obj_set_style_border_color(s->sub_box, lv_color_hex(C_ACCENT), 0);
    s->sub_clip = plain(s->sub_box);
    lv_obj_set_size(s->sub_clip, 202, 70);
    lv_obj_center(s->sub_clip);
    s->sub_lbl = lv_label_create(s->sub_clip);
    lv_obj_set_width(s->sub_lbl, 200);
    lv_label_set_long_mode(s->sub_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s->sub_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s->sub_lbl, SUB_SPACE, 0);
    lv_obj_set_style_text_font(s->sub_lbl, f16, 0);
    lv_obj_set_style_text_color(s->sub_lbl, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(s->sub_lbl, "");
    s->sub_img = lv_image_create(s->sub_clip);
    lv_obj_remove_flag(s->sub_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s->sub_box, LV_OBJ_FLAG_HIDDEN);

    // 对话框
    s->box = lv_obj_create(s->scr);
    lv_obj_remove_style_all(s->box);
    lv_obj_set_pos(s->box, 10, 182);
    lv_obj_set_size(s->box, 220, 130);
    lv_obj_set_style_radius(s->box, 14, 0);
    lv_obj_set_style_bg_color(s->box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s->box, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s->box, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(s->box, 2, 0);
    lv_obj_set_scrollbar_mode(s->box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s->box, LV_OBJ_FLAG_HIDDEN);

    s->body = lv_label_create(s->box);
    lv_obj_set_pos(s->body, 6, 10);
    lv_obj_set_width(s->body, 208);  // 13 个 16 px 汉字，减少标点单独成行
    lv_label_set_long_mode(s->body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s->body, f16, 0);
    lv_obj_set_style_text_line_space(s->body, 1, 0);
    lv_obj_set_style_text_color(s->body, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(s->body, "");

    for (int i = 0; i < MAX_ROWS; i++) {
        s->row[i] = plain(s->box);
        lv_obj_set_pos(s->row[i], 8, 10 + i * 38);
        lv_obj_set_size(s->row[i], 200, 32);
        lv_obj_set_style_radius(s->row[i], 10, 0);
        lv_obj_set_style_bg_opa(s->row[i], LV_OPA_COVER, 0);
        s->row_lbl[i] = lv_label_create(s->row[i]);
        lv_obj_set_style_text_font(s->row_lbl[i], f16, 0);
        lv_label_set_long_mode(s->row_lbl[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s->row_lbl[i], 184);
        lv_obj_align(s->row_lbl[i], LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_add_flag(s->row[i], LV_OBJ_FLAG_HIDDEN);
    }

    s->more = lv_label_create(s->scr);  // 放在屏幕上而不是对话框里，不随正文滚动
    lv_obj_set_style_text_font(s->more, f14, 0);
    lv_obj_set_style_text_color(s->more, lv_color_hex(C_ACCENT), 0);
    lv_label_set_text(s->more, "");
    lv_obj_set_pos(s->more, 204, 290);
    lv_obj_add_flag(s->more, LV_OBJ_FLAG_HIDDEN);

    // 名牌压在对话框上沿
    lv_obj_t *tag = lv_label_create(s->scr);
    lv_obj_set_style_text_font(tag, f14, 0);
    lv_obj_set_style_text_color(tag, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(tag, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tag, 9, 0);
    lv_obj_set_style_pad_hor(tag, 9, 0);
    lv_obj_set_style_pad_ver(tag, 2, 0);
    lv_label_set_text(tag, "Claude娘");
    lv_obj_set_pos(tag, 18, 172);
    lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);

    s->hint = lv_label_create(s->scr);
    lv_obj_set_style_text_font(s->hint, f14, 0);
    lv_obj_set_style_text_color(s->hint, lv_color_hex(C_ACCENT), 0);
    lv_label_set_text(s->hint, "按确定暂停");
    lv_obj_align(s->hint, LV_ALIGN_BOTTOM_MID, 0, -9);
}

// ---- 生命周期 -------------------------------------------------------------

void claude_chan_enter(void) {
    if (s) return;
    s = cc_platform_alloc(sizeof(*s));
    if (!s) {
        ESP_LOGE(TAG, "内存不足");
        return;
    }
    memset(s, 0, sizeof(*s));
    s->full = cc_platform_alloc(BODY_CAP);
    s->sse = cc_platform_alloc(sizeof(cc_sse_t));
    if (!s->full || !s->sse) {
        ESP_LOGE(TAG, "内存不足");
        cc_platform_free(s->full);
        cc_platform_free(s->sse);
        cc_platform_free(s);
        s = NULL;
        return;
    }
    s->full[0] = '\0';
    s->req_id = -1;
    cc_history_init(&s->hist);
    reset_reply_parsers();

    build_ui();
    lv_screen_load(s->scr);
    s->last_charging = cc_platform_charging();
    s->last_soc = cc_platform_battery_soc();
    s->low_batt_notified = (s->last_soc >= 0 && s->last_soc <= 20);  // 开机就低电量时由问候自然提到
    update_status_bar();

    s->shown_phase = -1;
    s->type_timer = lv_timer_create(type_cb, 45, NULL);
    s->dots_timer = lv_timer_create(dots_cb, 350, NULL);
    s->tick_timer = lv_timer_create(tick_cb, 1000, NULL);
    s->phase_timer = lv_timer_create(phase_cb, 40, NULL);

    ESP_LOGI(TAG, "进入 Claude娘，模型=%s", cc_platform_model());
    set_state(ST_IDLE);
}

void claude_chan_exit(void) {
    if (!s) return;
    s->gen++;
    if (s->req_id >= 0) cc_platform_http_cancel(s->req_id);
    lv_timer_delete(s->type_timer);
    lv_timer_delete(s->dots_timer);
    lv_timer_delete(s->tick_timer);
    lv_timer_delete(s->phase_timer);
    cc_avatar_delete(s->avatar);
    // 屏幕本身由应用框架在 exit 返回后删除（与官方 demo 的约定一致）
    cc_history_clear(&s->hist);
    cc_platform_free(s->pending_user);
    cc_platform_free(s->full);
    cc_platform_free(s->sse);
    cc_platform_free(s->sub_a8);
    cc_platform_free(s);
    s = NULL;
    ESP_LOGI(TAG, "退出 Claude娘");
}

void claude_chan_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s) return;
    if (ev == BSP_BTN_DOUBLE && btn == BSP_BTN_OK) {
        cc_avatar_pat(s->avatar);
        if (!s->voice_hold) cc_avatar_set_emotion(s->avatar, CC_EMO_SHY);
        return;
    }
    if (ev != BSP_BTN_CLICK || btn != BSP_BTN_OK) return;
    bool hold = !s->voice_hold;
    cc_platform_voice_hold(hold);
    apply_hold(hold);
}
