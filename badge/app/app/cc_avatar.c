// cc_avatar.c — 见 cc_avatar.h。
//
// 立绘是按参考图生成的六张场景图（cc_sprites.c）。说话时用 speak，
// 其余表情各用一张。图像在 flash 里，不占 RAM。
#include "cc_avatar.h"

#include <string.h>

#include "cc_platform.h"
#include "cc_sprites.h"

struct cc_avatar {
    lv_obj_t *root;
    lv_obj_t *img;
    lv_obj_t *mark;  // 头顶的小符号
    const lv_image_dsc_t *shown;
    int32_t base_y;
    cc_emotion_t emotion;
    bool talking;
    bool listening;
};

typedef struct {
    const lv_image_dsc_t *sprite;
    const char *mark;  // NULL = 不显示
    uint32_t color;
} look_t;

static look_t look_for(cc_emotion_t emo, bool talking, bool listening) {
    if (talking) return (look_t){&cc_sprite_speak, NULL, 0};
    if (listening) return (look_t){&cc_sprite_listen, "…", 0xE0784A};
    switch (emo) {
    case CC_EMO_HAPPY:     return (look_t){&cc_sprite_cheer, NULL, 0};
    case CC_EMO_PROUD:     return (look_t){&cc_sprite_cheer, "★", 0xF2A33A};
    case CC_EMO_SURPRISED: return (look_t){&cc_sprite_speak, "！", 0xE5484D};
    case CC_EMO_THINKING:  return (look_t){&cc_sprite_think, NULL, 0};
    case CC_EMO_SHY:       return (look_t){&cc_sprite_worry, "〃", 0xFF7A8A};
    case CC_EMO_SAD:       return (look_t){&cc_sprite_worry, NULL, 0};
    case CC_EMO_ANGRY:     return (look_t){&cc_sprite_worry, "＃", 0xE5484D};
    case CC_EMO_SLEEPY:    return (look_t){&cc_sprite_sleep, NULL, 0};
    case CC_EMO_CALM:
    default:               return (look_t){&cc_sprite_listen, NULL, 0};
    }
}

static void apply_emotion(cc_avatar_t *a) {
    look_t look = look_for(a->emotion, a->talking, a->listening);
    if (look.sprite != a->shown) {
        a->shown = look.sprite;
        lv_image_set_src(a->img, look.sprite);
        lv_obj_align(a->img, LV_ALIGN_TOP_MID, 0, 0);
    }
    if (look.mark) {
        lv_label_set_text(a->mark, look.mark);
        lv_obj_set_style_text_color(a->mark, lv_color_hex(look.color), 0);
        lv_obj_remove_flag(a->mark, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->mark, LV_OBJ_FLAG_HIDDEN);
    }
}

static void y_exec(void *obj, int32_t v) {
    lv_obj_set_y(obj, v);
}

static void ty_exec(void *obj, int32_t v) {
    lv_obj_set_style_translate_y(obj, v, 0);
}

static void tx_exec(void *obj, int32_t v) {
    lv_obj_set_style_translate_x(obj, v, 0);
}

// 从当前位置缓动到 to。同一属性上的旧动画先停掉，所以切换状态时不会跳。
static void glide(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms) {
    lv_anim_delete(obj, exec);
    if (from == to) {
        exec(obj, to);
        return;
    }
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, obj);
    lv_anim_set_exec_cb(&an, exec);
    lv_anim_set_values(&an, from, to);
    lv_anim_set_duration(&an, ms);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
    lv_anim_start(&an);
}

static void pat_done(lv_anim_t *an) {
    lv_obj_t *img = an->var;
    glide(img, tx_exec, lv_obj_get_style_translate_x(img, 0), 0, 160);
}

// 动效原则：只做 1～3 px 的缓动，不做定时器硬切的位移。
// 以前说话是每 140 ms 上下跳 8 px、听的时候每 160 ms 左右甩 8 px，看起来像在蹦。
// 现在说话靠换张嘴的图 + 很轻的起伏，听的时候慢慢侧过身去然后停住。

static void start_talk_motion(cc_avatar_t *a) {
    lv_anim_delete(a->img, ty_exec);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, a->img);
    lv_anim_set_exec_cb(&an, ty_exec);
    lv_anim_set_values(&an, lv_obj_get_style_translate_y(a->img, 0), -2);
    lv_anim_set_duration(&an, 460);
    lv_anim_set_reverse_duration(&an, 460);
    lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
    lv_anim_start(&an);
}

cc_avatar_t *cc_avatar_create(lv_obj_t *parent, int32_t x, int32_t y) {
    cc_avatar_t *a = cc_platform_alloc(sizeof(*a));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    a->base_y = y;

    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, CC_AVATAR_W, CC_AVATAR_H);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->root = r;

    a->img = lv_image_create(r);
    lv_obj_remove_flag(a->img, LV_OBJ_FLAG_CLICKABLE);

    a->mark = lv_label_create(r);
    lv_obj_set_style_text_font(a->mark, cc_platform_font(16), 0);
    lv_obj_set_pos(a->mark, 22, 6);  // 头的左上方，避开右侧的花饰

    a->emotion = CC_EMO_CALM;
    apply_emotion(a);

    // 呼吸浮动：2 px，很慢。值只在整数变化时才触发重绘。
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, r);
    lv_anim_set_exec_cb(&an, y_exec);
    lv_anim_set_values(&an, y, y - 2);
    lv_anim_set_duration(&an, 2600);
    lv_anim_set_reverse_duration(&an, 2600);
    lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
    lv_anim_start(&an);
    return a;
}

void cc_avatar_delete(cc_avatar_t *a) {
    if (!a) return;
    lv_anim_delete(a->root, NULL);
    lv_anim_delete(a->img, NULL);
    lv_obj_delete(a->root);
    cc_platform_free(a);
}

void cc_avatar_set_emotion(cc_avatar_t *a, cc_emotion_t emo) {
    if (!a) return;
    a->emotion = emo;
    apply_emotion(a);
}

cc_emotion_t cc_avatar_get_emotion(const cc_avatar_t *a) {
    return a ? a->emotion : CC_EMO_CALM;
}

static void stop_talk(cc_avatar_t *a) {
    a->talking = false;
    glide(a->img, ty_exec, lv_obj_get_style_translate_y(a->img, 0), 0, 240);
}

static void stop_listen(cc_avatar_t *a) {
    a->listening = false;
    glide(a->img, tx_exec, lv_obj_get_style_translate_x(a->img, 0), 0, 320);
}

void cc_avatar_set_talking(cc_avatar_t *a, bool talking) {
    if (!a || a->talking == talking) return;
    if (talking) {
        if (a->listening) stop_listen(a);
        a->talking = true;
        apply_emotion(a);
        start_talk_motion(a);
    } else {
        stop_talk(a);
        apply_emotion(a);
    }
}

void cc_avatar_set_listening(cc_avatar_t *a, bool listening) {
    if (!a || a->listening == listening) return;
    if (listening) {
        if (a->talking) stop_talk(a);
        a->listening = true;
        apply_emotion(a);
        // 侧过身来听：3 px，慢慢过去，然后停住。
        glide(a->img, tx_exec, lv_obj_get_style_translate_x(a->img, 0), 3, 420);
    } else {
        stop_listen(a);
        apply_emotion(a);
    }
}

// 摸头：轻轻晃两下再回正。
void cc_avatar_pat(cc_avatar_t *a) {
    if (!a) return;
    lv_anim_delete(a->img, tx_exec);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, a->img);
    lv_anim_set_exec_cb(&an, tx_exec);
    lv_anim_set_values(&an, -2, 2);
    lv_anim_set_duration(&an, 180);
    lv_anim_set_reverse_duration(&an, 180);
    lv_anim_set_repeat_count(&an, 2);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&an, pat_done);
    lv_anim_start(&an);
}
