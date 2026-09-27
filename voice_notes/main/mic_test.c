/*
 * 阶段 1 验证固件：ICS-43434 接线与 I²S 配置
 *
 * 目的：在写任何产品代码之前，先证明"麦克风能出合理的数据"。
 *       这是 docs/prd.md 里标记为全局最高风险的一步 —— 64 SCK 帧长、
 *       MSB 延迟 1 拍、24bit 在 32bit 槽里的对齐，任何一个错都会表现为
 *       "能收音但听不清"，极难反推。
 *
 * 命令：
 *   rec [秒]    采集并打印统计量（对应 hardware.md §4 第 2 级验证）
 *   wav [秒]    裸流输出 16bit PCM，PC 侧存 WAV（对应第 3 级验证）
 *   shift [n]   改 24→16bit 的右移位数，默认 16（风险 R2 的诊断入口）
 *   cfg         重看当前配置
 *   help        帮助
 *
 * 配置依据：docs/prd.md §6.4
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_idf_version.h"

/* ------------------------------------------------------------------ */
/* 接线 —— 依据 docs/prd.md §6.3 / docs/decisions.md D10                */
/* 这三个脚避开了 GPIO6-11(Flash) / 0,2,12,15(strapping) / 1,3(UART)   */
/* ------------------------------------------------------------------ */
#define PIN_BCLK        GPIO_NUM_26     /* → ICS-43434 引脚 4 (SCK) */
#define PIN_WS          GPIO_NUM_25     /* → ICS-43434 引脚 1 (WS)  */
#define PIN_DIN         GPIO_NUM_22     /* ← ICS-43434 引脚 6 (SD)  */
/* LR 引脚(2) 接 GND → 数据出在左声道；VDD(5) 接 3V3 + 0.1µF；SD 加 100kΩ 下拉 */

#define SAMPLE_RATE     16000

/* 每次读 256 帧。上限推导：dma_buffer_size = frames × slots × slot_bits/8 ≤ 4092
 * → 256 × 2 × 32/8 = 2048 字节 ✓（若取 512 则 4096 > 4092，会失败） */
#define FRAMES_PER_READ 256
#define BYTES_PER_READ  (FRAMES_PER_READ * 2 * 4)   /* 2 槽 × 4 字节 = 2048 */

/* console 与裸 PCM 共用 UART0；裸流走 uart_write_bytes 绕过 stdio 的 \n→\r\n 转换 */
#define CONSOLE_UART    UART_NUM_0

static const char *TAG = "mic_test";

static i2s_chan_handle_t s_rx = NULL;
static const char *s_clk_name = "?";
static int s_shift = 16;    /* 24bit 数据在 32bit 字的 [31:8]，故 >>16 得 16bit */

/* ------------------------------------------------------------------ */
/* I²S 初始化                                                          */
/* ------------------------------------------------------------------ */
static esp_err_t i2s_init_once(bool use_apll)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = FRAMES_PER_READ;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_STEREO),   /* ⚠️ 必须 STEREO，见下方 §帧长 */
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,             /* ICS-43434 不需要 MCLK */
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (use_apll) {
        std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;   /* 精确音频时钟，见 NFR-3 */
    }

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    s_clk_name = use_apll ? "APLL" : "DEFAULT(PLL_D2)";
    return ESP_OK;
}

static void i2s_init_with_fallback(void)
{
    /* APLL 更精确，但它是共享资源，可能被别的外设占用 —— 失败就回退，
     * 并把实际用的是哪个时钟打出来，别让"以为用了 APLL"成为幻觉 */
    esp_err_t err = i2s_init_once(true);
    if (err == ESP_OK) return;

    ESP_LOGW(TAG, "APLL 初始化失败 (%s)，回退到默认时钟源", esp_err_to_name(err));
    err = i2s_init_once(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I²S 初始化彻底失败: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "检查：1) GPIO 22/25/26 是否被别的东西占用 2) 是否真的烧到这块板子");
    }
}

/* ------------------------------------------------------------------ */
/* 读一块，抽左声道并转 16bit                                          */
/* ------------------------------------------------------------------ */
static int read_block(int16_t *out, int n_frames)
{
    static uint8_t raw[BYTES_PER_READ];
    size_t got = 0;

    esp_err_t err = i2s_channel_read(s_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(1000));
    if (err != ESP_OK || got == 0) return -1;

    int frames = got / 8;               /* 每帧 8 字节 = 2 槽 × 32bit */
    if (frames > n_frames) frames = n_frames;

    const int32_t *w = (const int32_t *)raw;
    for (int i = 0; i < frames; i++) {
        /* 每帧两个 32bit 字：偶数下标 = 左声道（LR 接地那一路） */
        int32_t left = w[i * 2];
        out[i] = (int16_t)(left >> s_shift);
    }
    return frames;
}

/* ------------------------------------------------------------------ */
/* rec —— 采集并打印统计                                               */
/* ------------------------------------------------------------------ */
static void cmd_rec(int seconds)
{
    const int total = SAMPLE_RATE * seconds;
    int done = 0;
    int16_t buf[FRAMES_PER_READ];

    int16_t vmin = INT16_MAX, vmax = INT16_MIN;
    int64_t sum = 0, sum_sq = 0;
    int quiet = 0;

    printf("\n>>> 采集 %d 秒（现在可以说话）...\n", seconds);
    fflush(stdout);

    int64_t t0 = esp_log_timestamp();
    while (done < total) {
        int n = read_block(buf, FRAMES_PER_READ);
        if (n < 0) {
            printf("!! 读取失败，中止\n");
            return;
        }
        for (int i = 0; i < n; i++) {
            int16_t v = buf[i];
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
            sum += v;
            sum_sq += (int64_t)v * v;
            if (v > -100 && v < 100) quiet++;
        }
        done += n;
    }
    int64_t ms = esp_log_timestamp() - t0;

    double mean = (double)sum / done;
    double rms  = sqrt((double)sum_sq / done);
    double dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -999.0;
    double quiet_pct = 100.0 * quiet / done;

    printf("\n================ 采集统计 ================\n");
    printf("样本数     : %d  (用了 %" PRId64 " ms, 期望 %d ms)\n",
           done, ms, seconds * 1000);
    printf("最小值     : %d\n", vmin);
    printf("最大值     : %d\n", vmax);
    printf("峰峰值     : %d\n", vmax - vmin);
    printf("直流偏置   : %.1f   (应接近 0)\n", mean);
    printf("RMS        : %.1f\n", rms);
    printf("RMS 电平   : %.1f dBFS\n", dbfs);
    printf("静音样本比 : %.1f %%   (|样本| < 100)\n", quiet_pct);
    printf("==========================================\n");

    /* 自动判读 —— 直接把 hardware.md §4 的判据编码进来 */
    printf("判读: ");
    if (vmin == 0 && vmax == 0) {
        printf("❌ 全为 0 —— 没接到数据。查 SD 是否接对、VDD 是否供电、LR 是否接地\n");
    } else if (vmax - vmin < 200) {
        printf("❌ 幅度极小 —— 可能是位移方向错了。试 `shift 8`\n");
    } else if (quiet_pct < 1.0 && (vmax > 32000 || vmin < -32000)) {
        printf("❌ 满量程噪声 —— 位移过头或时钟太快。试 `shift 24`\n");
    } else if (quiet_pct > 99.0) {
        printf("⚠️  几乎全程静音 —— 数据通道通了但没收到声音。查 WS/SCK 是否接反\n");
    } else {
        printf("✅ 数值看起来正常（说话时峰值应落在 ±1000 ~ ±8000）\n");
    }
    printf("\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* wav —— 裸流输出 PCM，PC 侧存 WAV                                    */
/* ------------------------------------------------------------------ */
static void cmd_wav(int seconds)
{
    const int total = SAMPLE_RATE * seconds;
    int done = 0;
    int16_t buf[FRAMES_PER_READ];

    /* 绕过 stdio：直接写 UART，避免 \n → \r\n 转换污染二进制数据 */
    uart_wait_tx_done(CONSOLE_UART, pdMS_TO_TICKS(1000));
    char hdr[64];
    int hlen = snprintf(hdr, sizeof(hdr), "WAV_BEGIN %d %d\n", total, SAMPLE_RATE);
    uart_write_bytes(CONSOLE_UART, hdr, hlen);
    uart_wait_tx_done(CONSOLE_UART, pdMS_TO_TICKS(1000));

    while (done < total) {
        int n = read_block(buf, FRAMES_PER_READ);
        if (n < 0) break;
        uart_write_bytes(CONSOLE_UART, (const char *)buf, n * 2);
        done += n;
    }
    uart_wait_tx_done(CONSOLE_UART, pdMS_TO_TICKS(2000));
    uart_write_bytes(CONSOLE_UART, "\nWAV_END\n", 9);

    printf("已发送 %d 个样本（%d 字节 PCM）\n", done, done * 2);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
static void print_cfg(void)
{
    printf("\n================ 当前配置 ================\n");
    printf("采样率     : %d Hz\n", SAMPLE_RATE);
    printf("时钟源     : %s\n", s_clk_name);
    printf("位宽/槽    : 32 bit × 2 槽 = 64 SCK/帧\n");
    printf("  ↑ ICS-43434 要求每 WS 帧正好 64 个 SCK（数据手册 C2）\n");
    printf("  ↑ 若用 I2S_SLOT_MODE_MONO 会变成 32 SCK/帧，麦克风直接不工作\n");
    printf("BCLK 期望  : %.3f MHz  (64 × %d)\n", 64.0 * SAMPLE_RATE / 1e6, SAMPLE_RATE);
    printf("WS   期望  : %.3f kHz\n", SAMPLE_RATE / 1000.0);
    printf("右移位数   : %d   (24bit 数据在 32bit 字的 [31:8])\n", s_shift);
    printf("DMA        : 4 缓冲 × %d 帧 = %d ms 延迟\n",
           FRAMES_PER_READ, FRAMES_PER_READ * 1000 / SAMPLE_RATE * 4);
    printf("引脚       : BCLK=IO%d  WS=IO%d  DIN=IO%d\n",
           PIN_BCLK, PIN_WS, PIN_DIN);
    printf("控制台     : UART%d @ %d 8N1\n", CONSOLE_UART, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    printf("==========================================\n\n");
    fflush(stdout);
}

static void print_help(void)
{
    printf("\n命令:\n");
    printf("  rec [秒]    采集并打印统计量          (默认 3 秒)\n");
    printf("  wav [秒]    裸流输出 PCM，PC 侧存 WAV (默认 5 秒)\n");
    printf("  shift [n]   改 24→16bit 右移位数      (默认 16)\n");
    printf("  cfg         重看当前配置\n");
    printf("  help        本帮助\n\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
void app_main(void)
{
    printf("\n\n=== voice_notes 阶段 1：ICS-43434 采音验证 ===\n");
    printf("IDF %s | ESP32-PICO-V3-02\n", esp_get_idf_version());

    i2s_init_with_fallback();
    print_cfg();
    print_help();

    static char line[128];
    while (1) {
        printf("mic> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* console 默认非阻塞；空转时让出 CPU。
             * 注意：这里不做 hello_world 里的阻塞改造 —— 本固件只用
             * rec/wav 两个命令，非阻塞完全够用，少一层出错的可能。 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        int arg = 0;
        char cmd[32] = {0};
        sscanf(line, "%31s %d", cmd, &arg);

        if (strcmp(cmd, "rec") == 0) {
            cmd_rec(arg > 0 ? arg : 3);
        } else if (strcmp(cmd, "wav") == 0) {
            cmd_wav(arg > 0 ? arg : 5);
        } else if (strcmp(cmd, "shift") == 0) {
            if (arg >= 0 && arg <= 31) {
                s_shift = arg;
                printf("右移位数 → %d\n", s_shift);
            } else {
                printf("shift 取值范围 0..31\n");
            }
            fflush(stdout);
        } else if (strcmp(cmd, "cfg") == 0) {
            print_cfg();
        } else if (strcmp(cmd, "help") == 0) {
            print_help();
        } else {
            printf("未知命令: %s（敲 help 看用法）\n", cmd);
            fflush(stdout);
        }
    }
}
