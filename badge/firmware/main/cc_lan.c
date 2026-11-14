// cc_lan.c — 见 cc_lan.h。帧格式与 passport-sim/voice-mvp/lan_proto.py 一致。
#include "cc_lan.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wifi_secret.h"

#define FRAME_BYTES 640
#define HDR_BYTES 10
#define MAGIC 0xC3A1
#define TYPE_HELLO 1
#define TYPE_HELLO_ACK 2
#define TYPE_PCM 0x10
#define TYPE_CTRL 0x11
#define TYPE_TEXT 0x12   // Mac → 徽章的字幕：角色 u8 + UTF-8 全文
#define TEXT_CAP 256
#define TYPE_SUBIMG 0x13 // Mac → 徽章的字幕位图（Mac 排版好，徽章只贴图），见 cc_lan.h
#define SUBIMG_MAX_W 208
#define SUBIMG_MAX_H 72
#define CAP_SUBIMG 0x01  // HELLO 第 6 字节：本固件能收字幕位图

static const char *TAG = "cc_lan";
static int s_fd = -1;
static uint32_t s_tx_seq = 1;
static volatile int s_paused = 0;
static volatile int s_phase = 0;
static volatile int s_sent_pause = 0;
static volatile unsigned s_hold_ver = 0;
static portMUX_TYPE s_text_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_text[TEXT_CAP];
static int s_text_role = 0;
static unsigned s_text_ver = 0;

// 字幕位图（A4，一字节两个像素，高 4 位在左）。Mac 只发变了的行，最后一块带提交标志。
// 用序号锁：写的时候序号是奇数，读的一方发现奇数或前后序号不同就下一拍再读。
static uint8_t s_img[SUBIMG_MAX_H * (SUBIMG_MAX_W / 2)];
static volatile uint32_t s_img_seq = 0;
static int s_img_w, s_img_h, s_img_role;
static bool s_img_writing = false;

void cc_lan_set_paused(bool paused) {
    s_paused = paused ? 1 : 0;
}

int cc_lan_phase(void) {
    return s_phase;
}

unsigned cc_lan_text(char *buf, size_t cap, int *role) {
    taskENTER_CRITICAL(&s_text_mux);
    if (cap > 0) {
        size_t n = strnlen(s_text, sizeof(s_text));
        if (n >= cap) n = cap - 1;
        memcpy(buf, s_text, n);
        buf[n] = '\0';
    }
    *role = s_text_role;
    unsigned ver = s_text_ver;
    taskEXIT_CRITICAL(&s_text_mux);
    return ver;
}

unsigned cc_lan_remote_hold(bool *hold) {
    *hold = s_paused != 0;
    return s_hold_ver;
}

unsigned cc_lan_subimg(uint8_t *a8, size_t cap, int *w, int *h, int *role) {
    uint32_t s1 = __atomic_load_n(&s_img_seq, __ATOMIC_ACQUIRE);
    if (s1 == 0 || (s1 & 1)) return 0;
    int iw = s_img_w, ih = s_img_h, ir = s_img_role;
    if ((size_t)iw * (size_t)ih > cap) return 0;
    int stride = iw / 2;
    for (int y = 0; y < ih; y++) {
        const uint8_t *src = s_img + y * stride;
        uint8_t *dst = a8 + y * iw;
        for (int x = 0; x < stride; x++) {
            uint8_t b = src[x];
            dst[2 * x] = (uint8_t)((b >> 4) * 17);
            dst[2 * x + 1] = (uint8_t)((b & 0x0F) * 17);
        }
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&s_img_seq, __ATOMIC_ACQUIRE) != s1) return 0;
    *w = iw;
    *h = ih;
    *role = ir;
    return s1;
}

static void img_begin(void) {
    if (s_img_writing) return;
    s_img_writing = true;
    __atomic_add_fetch(&s_img_seq, 1, __ATOMIC_RELEASE);  // 变奇数
}

static void img_commit(void) {
    if (!s_img_writing) return;
    s_img_writing = false;
    __atomic_add_fetch(&s_img_seq, 1, __ATOMIC_RELEASE);  // 变偶数
}

// 载荷：角色 u8，标志 u8（bit0 提交），宽 u8，高 u8，起始行 u8，行数 u8，然后是 A4 像素。
static void on_subimg(const uint8_t *p, size_t n) {
    if (n < 6) return;
    int role = p[0], last = p[1] & 1, w = p[2], h = p[3], y0 = p[4], rows = p[5];
    int stride = w / 2;
    if (w > SUBIMG_MAX_W || h > SUBIMG_MAX_H || (w & 1) || y0 + rows > h) return;
    if (n < 6 + (size_t)rows * (size_t)stride) return;
    img_begin();
    if (w != s_img_w || h != s_img_h) memset(s_img, 0, sizeof(s_img));
    s_img_w = w;
    s_img_h = h;
    s_img_role = role;
    if (rows > 0) memcpy(s_img + y0 * stride, p + 6, (size_t)rows * (size_t)stride);
    if (last) img_commit();
}

static void set_text(int role, const uint8_t *utf8, size_t n) {
    if (n > TEXT_CAP - 1) n = TEXT_CAP - 1;  // Mac 只发最后 240 字节，正常到不了这里
    taskENTER_CRITICAL(&s_text_mux);
    if (n > 0) memcpy(s_text, utf8, n);
    s_text[n] = '\0';
    s_text_role = role;
    s_text_ver++;
    taskEXIT_CRITICAL(&s_text_mux);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int send_all(const void *data, size_t n) {
    const uint8_t *p = data;
    while (n > 0) {
        int w = send(s_fd, p, n, 0);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            return -1;
        }
        if (w == 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int send_frame(uint8_t kind, const void *payload, uint16_t nbytes) {
    uint8_t hdr[HDR_BYTES];
    put_u16(hdr, MAGIC);
    hdr[2] = kind;
    hdr[3] = 0;
    put_u32(hdr + 4, s_tx_seq++);
    put_u16(hdr + 8, nbytes);
    if (send_all(hdr, HDR_BYTES) != 0) return -1;
    if (nbytes > 0 && send_all(payload, nbytes) != 0) return -1;
    return 0;
}

static int connect_mac(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CC_LAN_PORT);
    if (inet_pton(AF_INET, CC_LAN_HOST, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static int recv_some(uint8_t *dst, size_t cap) {
    int n = recv(s_fd, dst, cap, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return n;
}

static void drop_front(uint8_t *buf, size_t *len, size_t n) {
    memmove(buf, buf + n, *len - n);
    *len -= n;
}

static void parse_frames(uint8_t *buf, size_t *len);

// 把套接字里现有的数据读空并拆帧。以前每轮只读 1 KB，读得比来得慢时，
// 没读走的小包一直占着 Wi-Fi 接收缓冲。返回 false 表示对端断开。
static bool drain(uint8_t *buf, size_t *len) {
    for (int round = 0; round < 8; round++) {
        size_t room = 4096 - *len;
        if (room < 1024) {
            parse_frames(buf, len);
            room = 4096 - *len;
            if (room < 1024) {
                *len = 0;  // 解析不动：流已错位，丢掉重新找帧头
                room = 4096;
            }
        }
        int n = recv_some(buf + *len, room);
        if (n < 0) return false;
        if (n == 0) break;
        *len += (size_t)n;
        parse_frames(buf, len);
    }
    return true;
}

int cc_lan_backlog(void) {
    int fd = s_fd;
    if (fd < 0) return -1;
    int n = 0;
    return lwip_ioctl(fd, FIONREAD, &n) == 0 ? n : -1;
}

static void parse_frames(uint8_t *buf, size_t *len) {
    while (*len >= HDR_BYTES) {
        if (get_u16(buf) != MAGIC) {
            drop_front(buf, len, 1);
            continue;
        }
        uint16_t nbytes = get_u16(buf + 8);
        if ((size_t)HDR_BYTES + nbytes > 4096) {
            drop_front(buf, len, 1);
            continue;
        }
        if (*len < (size_t)HDR_BYTES + nbytes) break;
        uint8_t kind = buf[2];
        const uint8_t *payload = buf + HDR_BYTES;
        if (kind == TYPE_PCM && nbytes == FRAME_BYTES && !s_paused) {
            bsp_audio_write(payload, FRAME_BYTES);
        } else if (kind == TYPE_CTRL && nbytes >= 2 && payload[0] == 0xA0) {
            s_phase = payload[1];
        } else if (kind == TYPE_CTRL && nbytes >= 2 && payload[0] == 0xA1) {
            // Mac 上点了暂停/继续。记成已同步，免得再发 CTRL 回去。
            int v = payload[1] ? 1 : 0;
            s_paused = v;
            s_sent_pause = v;
            s_hold_ver++;
        } else if (kind == TYPE_TEXT && nbytes >= 1) {
            set_text(payload[0], payload + 1, nbytes - 1u);
        } else if (kind == TYPE_SUBIMG) {
            on_subimg(payload, nbytes);
        }
        drop_front(buf, len, (size_t)HDR_BYTES + nbytes);
    }
}

static void lan_task(void *arg) {
    (void)arg;
    if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败");
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume(80);
    static uint8_t mic[FRAME_BYTES];
    static uint8_t rx[4096];
    size_t rx_len = 0;
    uint8_t hello[6];
    put_u32(hello, 16000);
    hello[4] = 1;
    hello[5] = CAP_SUBIMG;

    for (;;) {
        s_fd = connect_mac();
        if (s_fd < 0) {
            ESP_LOGW(TAG, "连不上 %s:%d，2 秒后重试", CC_LAN_HOST, CC_LAN_PORT);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        s_tx_seq = 1;
        rx_len = 0;
        if (send_frame(TYPE_HELLO, hello, sizeof(hello)) != 0) {
            close(s_fd);
            s_fd = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "已连接 %s:%d", CC_LAN_HOST, CC_LAN_PORT);
        bool alive = true;
        s_sent_pause = 0;
        while (alive) {
            int want = s_paused;
            if (want != s_sent_pause) {
                uint8_t flag = want ? 1 : 0;
                if (send_frame(TYPE_CTRL, &flag, 1) != 0) {
                    alive = false;
                    break;
                }
                s_sent_pause = want;
                ESP_LOGI(TAG, "%s", want ? "语音已暂停" : "语音继续");
            }
            if (bsp_audio_read(mic, FRAME_BYTES) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            // 暂停时仍把麦克风读走，避免 I2S 缓冲区堆满，但不送出去。
            if (!want && send_frame(TYPE_PCM, mic, FRAME_BYTES) != 0) alive = false;
            if (alive && !drain(rx, &rx_len)) alive = false;
        }
        ESP_LOGW(TAG, "连接断开");
        s_phase = 0;
        set_text(0, NULL, 0);
        img_begin();
        s_img_role = 0;
        s_img_h = 0;
        img_commit();
        close(s_fd);
        s_fd = -1;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool cc_lan_start(void) {
    if (xTaskCreate(lan_task, "cc_lan", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        return false;
    }
    return true;
}
