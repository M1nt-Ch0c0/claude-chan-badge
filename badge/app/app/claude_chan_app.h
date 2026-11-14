// claude_chan_app.h — Claude娘 应用入口，形状与官方 demo_entry_t 一致。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void claude_chan_enter(void);
void claude_chan_exit(void);
void claude_chan_key(bsp_btn_t btn, bsp_btn_ev_t ev);

#ifdef __cplusplus
}
#endif
