// cc_net.c — Wi-Fi STA + esp_http_client 流式 POST。日志里不出现密码和 key。
#include "cc_net.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bsp_display.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_secret.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "cc_net";
static EventGroupHandle_t s_wifi;
static bool s_ready;
static volatile int s_cancel_id;
static int s_active_id;
static int s_next_id = 1;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ready = false;
        ESP_LOGW(TAG, "Wi-Fi 断开，准备重连");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_ready = true;
        if (s_wifi) xEventGroupSetBits(s_wifi, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "已拿到 IP");
    }
}

bool cc_wifi_ready(void) {
    return s_ready;
}

bool cc_wifi_start(int timeout_ms) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除后重建（只影响 IDF NVS，不动 cardid）");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败");
        return false;
    }
    s_wifi = xEventGroupCreate();
    if (!s_wifi) return false;
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CC_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CC_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    // 语音是持续的 20 ms 小包流。省电模式下徽章在信标之间睡觉，路由器把包攒着每 100 ms 左右一股脑送来，
    // 每个小包各占一块约 1.6 KB 的 Wi-Fi 接收缓冲，堆会被吃光（2026-11-14 实测从 50 KB 掉到 3 KB 后重启）。
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();
    ESP_LOGI(TAG, "正在连接 %s", CC_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) return true;
    ESP_LOGW(TAG, "Wi-Fi 超时，改走 USB 桥");
    return false;
}

typedef struct {
    int id;
    char *url;
    char *body;
    size_t body_len;
    char **headers;
    cc_http_body_cb on_body;
    cc_http_done_cb on_done;
    void *user;
} https_job_t;

static void free_job(https_job_t *job) {
    free(job->url);
    free(job->body);
    if (job->headers) {
        for (char **h = job->headers; *h; h++) free(*h);
        free(job->headers);
    }
    free(job);
}

static void https_task(void *arg) {
    https_job_t *job = arg;
    int status = 0;
    int err = 0;
    esp_http_client_config_t cfg = {
        .url = job->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 90000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        err = -1;
        goto done;
    }
    esp_http_client_set_header(client, "x-api-key", CC_API_KEY);
    for (char **h = job->headers; h && *h; h++) {
        char *colon = strchr(*h, ':');
        if (!colon) continue;
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ') value++;
        if (strcasecmp(*h, "x-api-key") == 0 || strcasecmp(*h, "authorization") == 0) {
            *colon = ':';
            continue;
        }
        esp_http_client_set_header(client, *h, value);
        *colon = ':';
    }
    if (esp_http_client_open(client, job->body_len) != ESP_OK ||
        esp_http_client_write(client, job->body, job->body_len) < 0) {
        err = -1;
        goto done;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        err = -1;
        goto done;
    }
    status = esp_http_client_get_status_code(client);
    char buf[512];
    while (job->id != s_cancel_id) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            err = -1;
            break;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (bsp_lvgl_lock(2000)) {
            if (job->id != s_cancel_id && job->on_body) job->on_body(job->user, (const uint8_t *)buf, (size_t)n);
            bsp_lvgl_unlock();
        }
    }
done:
    if (client) esp_http_client_cleanup(client);
    if (job->id == s_active_id) s_active_id = 0;
    if (job->on_done && bsp_lvgl_lock(2000)) {
        job->on_done(job->user, status, err);
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "HTTPS %s status=%d err=%d", job->url, status, err);
    free_job(job);
    vTaskDelete(NULL);
}

int cc_https_post(const char *path, const char *const *headers, const char *body, size_t body_len,
                  cc_http_body_cb on_body, cc_http_done_cb on_done, void *user) {
    if (!s_ready || !path || !body) return -1;
    https_job_t *job = calloc(1, sizeof(*job));
    if (!job) return -1;
    size_t url_len = strlen(CC_API_BASE) + strlen(path) + 1;
    job->url = malloc(url_len);
    job->body = malloc(body_len + 1);
    if (!job->url || !job->body) {
        free_job(job);
        return -1;
    }
    snprintf(job->url, url_len, "%s%s", CC_API_BASE, path);
    memcpy(job->body, body, body_len);
    job->body[body_len] = '\0';
    job->body_len = body_len;
    int nheaders = 0;
    for (const char *const *h = headers; h && *h; h++) nheaders++;
    job->headers = calloc((size_t)nheaders + 1, sizeof(char *));
    if (!job->headers) {
        free_job(job);
        return -1;
    }
    for (int i = 0; i < nheaders; i++) job->headers[i] = strdup(headers[i]);
    job->on_body = on_body;
    job->on_done = on_done;
    job->user = user;
    job->id = s_next_id++;
    s_active_id = job->id;
    // TLS 握手要比较大的栈。
    if (xTaskCreate(https_task, "cc_https", 16384, job, 4, NULL) != pdPASS) {
        free_job(job);
        return -1;
    }
    return job->id;
}

void cc_https_cancel(int id) {
    s_cancel_id = id;
}
