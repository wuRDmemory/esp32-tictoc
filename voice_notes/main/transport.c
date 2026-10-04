/*
 * 串口传输：帧的收发。
 *
 * ⚠️ 本模块支持两条物理通路，由 board.h 的 BOARD_TRANSPORT_USB 选择：
 *
 *   BOARD_TRANSPORT_USB = 0 → UART0（经典 ESP32，经 CP2102N）
 *   BOARD_TRANSPORT_USB = 1 → USB-Serial-JTAG（S3 原生 USB）
 *
 * **必须与 console 走同一条通路**，否则会出现「printf 能出、但收不到命令、
 * 也发不出音频」—— 实测在 S3 上踩过：transport 写死 UART_NUM_0，而 S3 的
 * console 是 USB-Serial-JTAG，两者是**不同外设**，数据各走各的，永远碰不上。
 *
 * 两条通路共用同一套帧协议（见 audio_frame.h），所以上层无感。
 */
#include "transport.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "board.h"
#include "audio_frame.h"

#if BOARD_TRANSPORT_USB
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

#define TX_BUF_SIZE    8192     /* 约 250ms 音频，吸收 PC 端短暂卡顿 */
#define RX_BUF_SIZE    1024
#define TX_MAX_FRAME   (FRAME_OVERHEAD + FRAME_MAX_PAYLOAD)
#define TEXT_LINE_MAX  128

static uint32_t s_tx_frames, s_rx_frames, s_bad_crc;
static uint32_t s_tx_blocked_ms;

/* RX 侧状态：帧累积缓冲 + 文本行缓冲 */
static uint8_t s_rxbuf[FRAME_OVERHEAD + FRAME_MAX_PAYLOAD];
static int     s_rxlen;
static uint8_t s_line[TEXT_LINE_MAX];
static int     s_linelen;

/* ------------------------------------------------------------------ */
/* 通路抽象 —— 上层只用这四个函数，不直接碰 uart_* / usb_serial_jtag_* */
/* ------------------------------------------------------------------ */
static esp_err_t link_init(void)
{
#if BOARD_TRANSPORT_USB
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = TX_BUF_SIZE,
        .rx_buffer_size = RX_BUF_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    /* 让 printf 走驱动的发送环形缓冲，stdout 才正常 */
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    return ESP_OK;
#else
    esp_err_t err = uart_driver_install(UART_NUM_0, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    /* 注意：本模块**不使用 stdin** —— 输入全走 link_read_byte，
     * 否则会和 VFS 抢同一份 RX 字节 */
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
    return ESP_OK;
#endif
}

static void link_write(const uint8_t *data, size_t len)
{
    /* 记录阻塞时长 —— 这是判断"有没有因为 PC 读得慢而丢音频"的唯一依据 */
    const int64_t t0 = esp_timer_get_time();
#if BOARD_TRANSPORT_USB
    usb_serial_jtag_write_bytes(data, len, portMAX_DELAY);
#else
    uart_write_bytes(UART_NUM_0, (const char *)data, len);
#endif
    s_tx_blocked_ms += (uint32_t)((esp_timer_get_time() - t0) / 1000);
}

static int link_read_byte(uint8_t *c, int timeout_ms)
{
#if BOARD_TRANSPORT_USB
    return usb_serial_jtag_read_bytes(c, 1, pdMS_TO_TICKS(timeout_ms));
#else
    return uart_read_bytes(UART_NUM_0, c, 1, pdMS_TO_TICKS(timeout_ms));
#endif
}

void transport_wait_tx_done(void)
{
#if BOARD_TRANSPORT_USB
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(2000));
#else
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(2000));
#endif
}

/* ------------------------------------------------------------------ */
esp_err_t transport_init(void)
{
    return link_init();
}

void transport_send(uint8_t type, const uint8_t *payload, uint16_t len)
{
    static uint8_t frame[TX_MAX_FRAME];

    size_t n = frame_pack(frame, sizeof(frame), type, payload, len);
    if (n == 0) return;

    link_write(frame, n);
    s_tx_frames++;
}

void transport_send_text(uint8_t type, const char *text)
{
    transport_send(type, (const uint8_t *)text, (uint16_t)strlen(text));
}

void transport_write_raw(const uint8_t *data, size_t len)
{
    link_write(data, len);
}

/* 处理文本行模式下的一个字节。返回 payload 长度或 -1 */
static int handle_text_byte(uint8_t ch, uint8_t *out_type, uint8_t *out_payload,
                            int max_len)
{
    if (ch == '\n' || ch == '\r') {
        if (s_linelen == 0) return -1;              /* 空行，忽略 */
        int len = s_linelen;
        if (len > max_len) len = max_len;
        memcpy(out_payload, s_line, len);
        out_payload[len] = '\0';
        out_type[0] = FRAME_TYPE_CONTROL;
        s_linelen = 0;
        s_rx_frames++;
        return len;
    }
    if (s_linelen < TEXT_LINE_MAX - 1) {
        s_line[s_linelen++] = ch;
    } else {
        s_linelen = 0;                              /* 超长行，丢弃重来 */
    }
    return -1;
}

int transport_poll(uint8_t *out_type, uint8_t *out_payload, int max_len, int timeout_ms)
{
    uint8_t ch;
    int n = link_read_byte(&ch, timeout_ms);
    if (n <= 0) return -1;

    /* ---- 文本行模式：帧从未开始时，任何非 0xAA 首字节都按文本处理 ---- */
    if (s_rxlen == 0 && ch != FRAME_MAGIC_0) {
        return handle_text_byte(ch, out_type, out_payload, max_len);
    }

    /* ---- 帧模式 ---- */
    if (s_rxlen >= (int)sizeof(s_rxbuf)) {          /* 防越界 */
        s_rxlen = 0;
        s_bad_crc++;
        return -1;
    }
    s_rxbuf[s_rxlen++] = ch;

    if (s_rxlen == 1) return -1;                    /* 收到 0xAA，等 0x55 */

    if (s_rxlen == 2 && ch != FRAME_MAGIC_1) {
        /* 不是帧头（假 magic）。丢掉 —— 文本命令不会以 0xAA 开头 */
        s_rxlen = 0;
        return -1;
    }

    if (s_rxlen < FRAME_OVERHEAD) return -1;        /* 头还没收全 */

    uint16_t plen = (uint16_t)(s_rxbuf[3] | (s_rxbuf[4] << 8));
    if (plen > FRAME_MAX_PAYLOAD) {                 /* 长度不合理，假同步 */
        s_rxlen = 0;
        s_bad_crc++;
        return -1;
    }

    size_t total = (size_t)FRAME_OVERHEAD + plen;
    if ((size_t)s_rxlen < total) return -1;         /* 体还没收全 */

    uint16_t want = (uint16_t)(s_rxbuf[5 + plen] | (s_rxbuf[5 + plen + 1] << 8));
    int ret = -1;
    if (crc16_ccitt(&s_rxbuf[2], 3 + plen) == want) {
        int len = plen > (uint16_t)max_len ? max_len : plen;
        memcpy(out_payload, &s_rxbuf[5], len);
        out_payload[len] = '\0';
        out_type[0] = s_rxbuf[2];
        s_rx_frames++;
        ret = len;
    } else {
        s_bad_crc++;
    }
    s_rxlen = 0;
    return ret;
}

void transport_get_stats(uint32_t *tx_frames, uint32_t *rx_frames, uint32_t *bad_crc)
{
    if (tx_frames) *tx_frames = s_tx_frames;
    if (rx_frames) *rx_frames = s_rx_frames;
    if (bad_crc)   *bad_crc   = s_bad_crc;
}

uint32_t transport_tx_blocked_ms(void) { return s_tx_blocked_ms; }

void transport_reset_counters(void)
{
    s_tx_frames = s_rx_frames = s_bad_crc = 0;
    s_tx_blocked_ms = 0;
}
