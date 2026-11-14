// cc_main.c — Claude娘 固件入口：BSP 初始化、按键任务、USB 桥，然后进入应用。
//
// 线程约定：应用代码（claude_chan_*）只在持有 LVGL 锁时被调用。
//   - lv_timer 回调：LVGL 任务里，本来就持锁
//   - 按键：按键回调（esp_timer 任务）只入队，key 任务取出后加锁再分发
//   - 网络回调：桥的接收任务加锁后调用
#include <stdio.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "cc_bridge.h"
#include "cc_lan.h"
#include "cc_net.h"
#include "claude_chan_app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "cc_main";

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_msg_t;

static QueueHandle_t s_keys;

static void post_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    key_msg_t m = {btn, ev};
    if (s_keys) xQueueSend(s_keys, &m, 0);
}

static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    post_key(btn, ev);
}

static void key_task(void *arg) {
    (void)arg;
    key_msg_t m;
    for (;;) {
        if (xQueueReceive(s_keys, &m, portMAX_DELAY) != pdTRUE) continue;
        if (!bsp_lvgl_lock(1000)) continue;
        claude_chan_key(m.btn, m.ev);
        bsp_lvgl_unlock();
    }
}

static void log_heap(const char *when) {
    ESP_LOGI(TAG, "堆 %s：空闲 %u 最低 %u 最大块 %u", when, (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// 每 15 秒记一次堆，空闲低于 24 KB 时报警，方便对照字幕/Wi-Fi 的内存占用。
static void heap_watch(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        size_t free_now = esp_get_free_heap_size();
        if (free_now < 24 * 1024) {
            ESP_LOGW(TAG, "堆偏低：空闲 %u 最低 %u 最大块 %u", (unsigned)free_now,
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        } else {
            log_heap("巡检");
        }
        ESP_LOGI(TAG, "网络积压 %d 字节", cc_lan_backlog());
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Claude娘 固件启动");
    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }
    lv_display_t *disp = bsp_lvgl_init();
    if (!disp) {
        ESP_LOGE(TAG, "LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(80);
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用");

    s_keys = xQueueCreate(8, sizeof(key_msg_t));
    if (!s_keys || xTaskCreate(key_task, "cc_keys", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败");
        return;
    }
    if (bsp_button_init(on_button, NULL) != ESP_OK) ESP_LOGW(TAG, "按键不可用");

    // 启动画面：等主机桥上线（最多 8 秒），这样开场问候能直接发出去
    lv_obj_t *splash = NULL;
    if (bsp_lvgl_lock(1000)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFF5EA), 0);
        splash = lv_label_create(scr);
        lv_obj_set_style_text_color(splash, lv_color_hex(0xE0784A), 0);
        lv_label_set_text(splash, "Claude-chan\nconnecting Wi-Fi...");
        lv_obj_set_style_text_align(splash, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(splash);
        bsp_lvgl_unlock();
    }
    log_heap("BSP 就绪");

    bool wifi = cc_wifi_start(20000);
    log_heap(wifi ? "Wi-Fi 已连接" : "Wi-Fi 未连接");
    if (wifi && !cc_lan_start()) ESP_LOGE(TAG, "局域网语音没启动");

    if (!cc_bridge_start(disp, post_key)) ESP_LOGE(TAG, "USB 桥启动失败");
    for (int i = 0; i < 80 && !cc_bridge_alive(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "主机桥%s", cc_bridge_alive() ? "已上线" : "未上线，先进入应用");

    if (bsp_lvgl_lock(2000)) {
        lv_obj_t *old = lv_screen_active();
        claude_chan_enter();
        if (lv_screen_active() != old) lv_obj_delete(old);
        bsp_lvgl_unlock();
    }
    log_heap("应用已进入");
    xTaskCreate(heap_watch, "cc_heap", 3072, NULL, 1, NULL);
}
