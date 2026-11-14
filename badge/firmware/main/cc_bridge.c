// cc_bridge.c — 见 cc_bridge.h。
#include "cc_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/base64.h"

#define BRIDGE_VERSION 1
#define LINE_MAX 1024
#define CHUNK_RAW 144  // 每行携带的原始字节数（base64 后 192 字符）
#define ALIVE_WINDOW_US (5 * 1000 * 1000)

static const char *TAG = "cc_bridge";

static struct _lv_display_t *s_disp;
static cc_bridge_key_cb s_on_key;
static volatile int64_t s_last_ping_us = -ALIVE_WINDOW_US;
static SemaphoreHandle_t s_tx_lock;  // 保证一条协议行不被别的协议行打断

static struct {
    int id;  // 0 = 空闲
    cc_bridge_body_cb on_body;
    cc_bridge_done_cb on_done;
    void *user;
} s_req;
static int s_next_id = 1;

static volatile bool s_shot_requested;
static bool s_shot_active;

// ---- 发送 -----------------------------------------------------------------

static void emit_line(const char *line) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    xSemaphoreGive(s_tx_lock);
}

static void emit_b64(const char *prefix, const uint8_t *data, size_t len) {
    static char line[64 + 4 * ((CHUNK_RAW + 2) / 3) + 8];  // 只在持 s_tx_lock 时使用
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    size_t off = 0;
    do {
        size_t n = len - off < CHUNK_RAW ? len - off : CHUNK_RAW;
        int p = snprintf(line, sizeof(line), "%s ", prefix);
        size_t out = 0;
        mbedtls_base64_encode((unsigned char *)line + p, sizeof(line) - (size_t)p, &out, data + off, n);
        line[p + out] = '\0';
        fputs(line, stdout);
        fputc('\n', stdout);
        off += n;
    } while (off < len);
    fflush(stdout);
    xSemaphoreGive(s_tx_lock);
}

bool cc_bridge_alive(void) {
    return esp_timer_get_time() - s_last_ping_us < ALIVE_WINDOW_US;
}

int cc_bridge_http_post(const char *path, const char *const *headers, const char *body, size_t body_len,
                        cc_bridge_body_cb on_body, cc_bridge_done_cb on_done, void *user) {
    if (!cc_bridge_alive()) {
        ESP_LOGW(TAG, "主机桥不在线，请求未发出");
        return -1;
    }
    int id = s_next_id++;
    s_req.id = id;
    s_req.on_body = on_body;
    s_req.on_done = on_done;
    s_req.user = user;

    char prefix[48];
    snprintf(prefix, sizeof(prefix), "@CC POST %d %s", id, path);
    emit_line(prefix);
    for (const char *const *h = headers; h && *h; h++) {
        snprintf(prefix, sizeof(prefix), "@CC H %d", id);
        emit_b64(prefix, (const uint8_t *)*h, strlen(*h));
    }
    snprintf(prefix, sizeof(prefix), "@CC B %d", id);
    emit_b64(prefix, (const uint8_t *)body, body_len);
    snprintf(prefix, sizeof(prefix), "@CC GO %d %u", id, (unsigned)body_len);
    emit_line(prefix);
    return id;
}

void cc_bridge_http_cancel(int id) {
    if (id <= 0 || s_req.id != id) return;
    s_req.id = 0;
    char line[32];
    snprintf(line, sizeof(line), "@CC CANCEL %d", id);
    emit_line(line);
}

// ---- 截屏：在 flush 事件里把每一行像素发给主机 ---------------------------

static void shot_flush_event(lv_event_t *event) {
    if (!s_shot_requested && !s_shot_active) return;
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!area || !buf || !buf->data) return;
    int32_t w = lv_area_get_width(area);
    if (!s_shot_active) {
        // 等整屏重绘从第一条带开始
        if (area->x1 != 0 || area->y1 != 0 || w != BSP_LCD_W) return;
        s_shot_requested = false;
        s_shot_active = true;
        char line[40];
        snprintf(line, sizeof(line), "@CC SHOT BEGIN %d %d", BSP_LCD_W, BSP_LCD_H);
        emit_line(line);
    }
    if (area->x1 != 0 || w != BSP_LCD_W) return;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        char prefix[24];
        snprintf(prefix, sizeof(prefix), "@CC PX %d", (int)y);
        const uint8_t *row = buf->data + (size_t)(y - area->y1) * buf->header.stride;
        // 一行 480 字节，拆成几段 base64；主机按 y 拼接
        emit_b64(prefix, row, (size_t)w * 2);
    }
    if (area->y2 >= BSP_LCD_H - 1) {
        s_shot_active = false;
        emit_line("@CC SHOT END");
    }
}

// ---- 接收 -----------------------------------------------------------------

static bool parse_btn(const char *s, bsp_btn_t *btn) {
    if (strcmp(s, "up") == 0) *btn = BSP_BTN_UP;
    else if (strcmp(s, "down") == 0) *btn = BSP_BTN_DOWN;
    else if (strcmp(s, "ok") == 0) *btn = BSP_BTN_OK;
    else return false;
    return true;
}

static bool parse_ev(const char *s, bsp_btn_ev_t *ev) {
    if (strcmp(s, "press") == 0) *ev = BSP_BTN_PRESS;
    else if (strcmp(s, "click") == 0) *ev = BSP_BTN_CLICK;
    else if (strcmp(s, "double") == 0) *ev = BSP_BTN_DOUBLE;
    else if (strcmp(s, "long") == 0) *ev = BSP_BTN_LONG;
    else if (strcmp(s, "release") == 0) *ev = (bsp_btn_ev_t)4;  // BSP_BTN_RELEASE；旧版 BSP 没有这个枚举名
    else return false;
    return true;
}

static void handle_line(char *line) {
    if (line[0] != '@') return;
    switch (line[1]) {
    case 'P':
        s_last_ping_us = esp_timer_get_time();
        break;
    case 'T': {
        long long epoch = 0;
        char tz[32] = "UTC0";
        if (sscanf(line + 2, "%lld %31s", &epoch, tz) >= 1 && epoch > 1600000000LL) {
            struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
            settimeofday(&tv, NULL);
            setenv("TZ", tz, 1);
            tzset();
            s_last_ping_us = esp_timer_get_time();
            ESP_LOGI(TAG, "时间已同步 epoch=%lld tz=%s", epoch, tz);
        }
        break;
    }
    case 'D': {
        int id = 0, off = 0;
        if (sscanf(line + 2, "%d %n", &id, &off) < 1 || id != s_req.id || id == 0) break;
        static uint8_t raw[LINE_MAX];
        size_t out = 0;
        const char *b64 = line + 2 + off;
        if (mbedtls_base64_decode(raw, sizeof(raw), &out, (const unsigned char *)b64, strlen(b64)) != 0) {
            ESP_LOGW(TAG, "响应分块 base64 无法解码");
            break;
        }
        if (bsp_lvgl_lock(2000)) {
            if (id == s_req.id && s_req.on_body) s_req.on_body(s_req.user, raw, out);
            bsp_lvgl_unlock();
        }
        break;
    }
    case 'E': {
        int id = 0, status = 0, err = 0;
        if (sscanf(line + 2, "%d %d %d", &id, &status, &err) < 3 || id != s_req.id || id == 0) break;
        cc_bridge_done_cb done = s_req.on_done;
        void *user = s_req.user;
        s_req.id = 0;
        if (done && bsp_lvgl_lock(2000)) {
            done(user, status, err);
            bsp_lvgl_unlock();
        }
        break;
    }
    case 'K': {
        char b[8], e[10];
        bsp_btn_t btn;
        bsp_btn_ev_t ev;
        if (sscanf(line + 2, "%7s %9s", b, e) == 2 && parse_btn(b, &btn) && parse_ev(e, &ev) && s_on_key) {
            s_on_key(btn, ev);
        }
        break;
    }
    case 'S':
        // 持锁期间 LVGL 不会刷新：先整屏失效再置标志，保证下一轮刷新就是完整的一帧
        if (bsp_lvgl_lock(1000)) {
            lv_obj_invalidate(lv_screen_active());
            s_shot_requested = true;
            bsp_lvgl_unlock();
        }
        break;
    case 'M': {
        char out[80];
        snprintf(out, sizeof(out), "@CC MEM %u %u %u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        emit_line(out);
        break;
    }
    case 'R':
        esp_restart();
        break;
    default:
        break;
    }
}

static void rx_task(void *arg) {
    (void)arg;
    static char line[LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    uint8_t buf[128];
    int64_t last_hello = 0;
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (len > 0 && !overflow) {
                    line[len] = '\0';
                    handle_line(line);
                }
                len = 0;
                overflow = false;
            } else if (len < LINE_MAX - 1) {
                line[len++] = c;
            } else {
                overflow = true;
            }
        }
        // 主机不在线时每秒打一次招呼，方便主机脚本发现设备并同步时间
        int64_t now = esp_timer_get_time();
        if (!cc_bridge_alive() && now - last_hello > 1000000) {
            last_hello = now;
            char hello[32];
            snprintf(hello, sizeof(hello), "@CC HELLO %d", BRIDGE_VERSION);
            emit_line(hello);
            // 请求进行中主机掉线：通知应用失败
            if (s_req.id != 0) {
                cc_bridge_done_cb done = s_req.on_done;
                void *user = s_req.user;
                s_req.id = 0;
                if (done && bsp_lvgl_lock(2000)) {
                    done(user, 0, -1);
                    bsp_lvgl_unlock();
                }
            }
        }
    }
}

bool cc_bridge_start(struct _lv_display_t *disp, cc_bridge_key_cb on_key) {
    s_disp = disp;
    s_on_key = on_key;
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) return false;

    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 4096, .rx_buffer_size = 4096};
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG 驱动安装失败");
        return false;
    }
    usb_serial_jtag_vfs_use_driver();

    if (bsp_lvgl_lock(1000)) {
        lv_display_add_event_cb(disp, shot_flush_event, LV_EVENT_FLUSH_START, NULL);
        bsp_lvgl_unlock();
    }
    if (xTaskCreate(rx_task, "cc_bridge_rx", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "接收任务创建失败");
        return false;
    }
    return true;
}
