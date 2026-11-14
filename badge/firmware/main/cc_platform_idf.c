// cc_platform_idf.c — Claude娘 的真机平台层（cc_platform.h 的 ESP-IDF 实现）。
//
// 联网走 USB 桥（cc_bridge）：设备不存任何凭据，主机脚本转发到中转服务。
// 字体是嵌进固件的 Noto Sans SC GB2312 子集（OFL），TinyTTF 直接从 flash 映射读取。
#include <stdlib.h>
#include <string.h>

#include "bsp_battery.h"
#include "cc_bridge.h"
#include "cc_lan.h"
#include "cc_net.h"
#include "cc_platform.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wifi_secret.h"

static const char *TAG = "cc_platform";

// 文字对话用的模型。在 wifi_secret.h 里定义 CC_MODEL 可以换掉（语音模式用不到）。
#ifndef CC_MODEL
#define CC_MODEL "claude-sonnet-5-5"
#endif

extern const uint8_t font_ttf_start[] asm("_binary_NotoSansSC_GB2312_ttf_start");
extern const uint8_t font_ttf_end[] asm("_binary_NotoSansSC_GB2312_ttf_end");

int cc_platform_http_post(const char *path, const char *const *headers, const char *body, size_t body_len,
                          cc_http_body_cb on_body, cc_http_done_cb on_done, void *user) {
    if (cc_wifi_ready()) return cc_https_post(path, headers, body, body_len, on_body, on_done, user);
    return cc_bridge_http_post(path, headers, body, body_len, on_body, on_done, user);
}

void cc_platform_http_cancel(int id) {
    cc_https_cancel(id);
    cc_bridge_http_cancel(id);
}

// 无 PSRAM：字形缓存按字号分别限量。语音模式的字幕已改成 Mac 渲染的位图，16 px 只在老的文字通路里用到。
// 每个缓存字形约 0.5 KB（字形信息 + A8 位图 + 两份缓存节点）。原来 16 px 是 96 个，
// 开 Wi-Fi 的语音模式下字幕会把它装满（约 48 KB），堆被吃到 20 多 KB，LVGL 申请失败后卡死。
// 字幕最多显示 3 行约 40 字，Mac 也只发最后约 40 字，48 个够用。
#define FONT_SLOTS 2
static struct {
    int px;
    lv_font_t *font;
} s_fonts[FONT_SLOTS];

const lv_font_t *cc_platform_font(int px) {
    for (int i = 0; i < FONT_SLOTS; i++) {
        if (s_fonts[i].font && s_fonts[i].px == px) return s_fonts[i].font;
    }
    for (int i = 0; i < FONT_SLOTS; i++) {
        if (s_fonts[i].font) continue;
        size_t cache = px >= 16 ? 48 : 24;
        int64_t t0 = esp_timer_get_time();
        lv_font_t *f = lv_tiny_ttf_create_data_ex(font_ttf_start, (size_t)(font_ttf_end - font_ttf_start), px,
                                                  LV_FONT_KERNING_NONE, cache);
        if (!f) {
            ESP_LOGE(TAG, "TinyTTF 创建 %d px 字体失败", px);
            return LV_FONT_DEFAULT;
        }
        ESP_LOGI(TAG, "字体 %d px 就绪，缓存 %u 个字形，用时 %lld us", px, (unsigned)cache,
                 (long long)(esp_timer_get_time() - t0));
        s_fonts[i].px = px;
        s_fonts[i].font = f;
        return f;
    }
    return s_fonts[0].font;  // 槽位用完：退回已有字号
}

int cc_platform_battery_soc(void) {
    return bsp_battery_soc();
}

// 板上没有充电状态引脚。USB 主机在线（收到 SOF）时视为在充电。
bool cc_platform_charging(void) {
    return usb_serial_jtag_is_connected();
}

void *cc_platform_alloc(size_t n) {
    return malloc(n);
}

void cc_platform_free(void *p) {
    free(p);
}

void cc_platform_voice_hold(bool hold) {
    cc_lan_set_paused(hold);
}

int cc_platform_voice_phase(void) {
    return cc_lan_phase();
}

unsigned cc_platform_voice_text(char *buf, size_t cap, int *role) {
    return cc_lan_text(buf, cap, role);
}

unsigned cc_platform_voice_remote_hold(bool *hold) {
    return cc_lan_remote_hold(hold);
}

unsigned cc_platform_voice_subimg(uint8_t *a8, size_t cap, int *w, int *h, int *role) {
    return cc_lan_subimg(a8, cap, w, h, role);
}

const char *cc_platform_model(void) {
    return CC_MODEL;
}
