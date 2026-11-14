// cc_avatar.h — Claude娘 的立绘：角色设定的 Q 版图（橙色长卷发、太阳花发饰、白色洋装）。
//
// 六张姿势图对应九种表情，另有头顶小符号、慢速呼吸、说话时轻微起伏、听时侧身、摸头轻晃。
// 所有位移都是 1～3 px 的缓动。
#pragma once

#include <stdbool.h>

#include "cc_reply.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CC_AVATAR_W 220
#define CC_AVATAR_H 164

typedef struct cc_avatar cc_avatar_t;

// 在 parent 上创建立绘（左上角放在 x, y）。返回的句柄由 cc_avatar_delete 释放。
cc_avatar_t *cc_avatar_create(lv_obj_t *parent, int32_t x, int32_t y);
void cc_avatar_delete(cc_avatar_t *a);

void cc_avatar_set_emotion(cc_avatar_t *a, cc_emotion_t emo);
cc_emotion_t cc_avatar_get_emotion(const cc_avatar_t *a);
// 说话时换张嘴的图，并轻轻起伏。
void cc_avatar_set_talking(cc_avatar_t *a, bool talking);
// 主人在说话时慢慢侧过身来听，然后停住。
void cc_avatar_set_listening(cc_avatar_t *a, bool listening);
// 摸头：轻晃两下再回正。
void cc_avatar_pat(cc_avatar_t *a);

#ifdef __cplusplus
}
#endif
