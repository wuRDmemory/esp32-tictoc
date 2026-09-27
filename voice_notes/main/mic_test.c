/*
 * 阶段 1 验证固件：ICS-43434 接线与 I²S 配置
 *
 * 目的：在写任何产品代码之前，先证明"麦克风能出合理的数据"。
 *       这是 docs/prd.md 里标记为全局最高风险的一步 —— 64 SCK 帧长、
 *       MSB 延迟 1 拍、24bit 在 32bit 槽里的对齐，任何一个错都会表现为
 *       "能收音但听不清"，极难反推。
 *
 * 命令：
 *   level [秒]  实时电平表 —— 验证麦克风最直接的手段（边说话边看数字）
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

/* 取哪一路。0 = 左声道（偶数下标），1 = 右声道（奇数下标）
 *
 * ⚠️ 为什么不一定是 0：模块上的 PS 跳线焊盘决定数据出在哪个声道。
 *    用户的模块（见 docs/modules/93545.pdf）默认「短接右边和中间 → 左声道」，
 *    但若被改成右声道，取左就会读到静音。与其让用户去焊板子，不如软件可切。 */
static int s_chan = 0;

/* 一阶直流阻断。MEMS 麦克风输出常带零点几个百分点的直流偏置（实测本模块
 * 约 413/32768 = 1.26% FS），不除掉会让 RMS、静音比这些统计量全部失真 ——
 * 表现是"静音比只有 1.6%"这种自相矛盾的数。产品链路本来也需要它。
 * y[n] = x[n] - x[n-1] + R*y[n-1]，R=0.995 时截止频率约 12.7 Hz @16kHz */
static bool s_dc_block = true;

/* ⚠️ 必须按声道分开保存滤波器状态。踩过的坑：最初两路共用 s_dc_x1/s_dc_y1，
 *    等于把左右声道当成一条交织的流在滤波 —— 右声道(SD 三态, 应恒为 0)会
 *    捡到左声道的残留状态而出现假信号，左声道的输出同样被污染。
 *    症状：右声道本该 [0,0]，却报出 [-336,-2]。 */
typedef struct { float x1, y1; } dcblk_t;
static dcblk_t s_dc[2];      /* [0]=左声道, [1]=右声道 */

/* 滤波前的原始均值，用于报告被滤掉的直流到底有多大 */
static int64_t s_raw_sum = 0, s_raw_n = 0;

static inline int16_t apply_dc_block(dcblk_t *d, int16_t x)
{
    if (!s_dc_block) return x;
    const float R = 0.995f;
    float y = (float)x - d->x1 + R * d->y1;
    d->x1 = (float)x;
    d->y1 = y;
    return (int16_t)y;
}

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
/* 读一块，两路声道都抽出来并转 16bit                                   */
/*                                                                     */
/* 两路都抽是刻意的：只有同时看到两路，才能区分「完全没数据」和          */
/* 「有数据但在另一个声道」—— 这两种故障的表现都是"取到静音"，          */
/* 但病因和修法完全不同。                                              */
/* ------------------------------------------------------------------ */
static int read_block(int16_t *left, int16_t *right, int n_frames)
{
    static uint8_t raw[BYTES_PER_READ];
    size_t got = 0;

    esp_err_t err = i2s_channel_read(s_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(1000));
    if (err != ESP_OK || got == 0) return -1;

    int frames = got / 8;               /* 每帧 8 字节 = 2 槽 × 32bit */
    if (frames > n_frames) frames = n_frames;

    const int32_t *w = (const int32_t *)raw;
    for (int i = 0; i < frames; i++) {
        int16_t l = (int16_t)(w[i * 2]     >> s_shift);
        int16_t r = (int16_t)(w[i * 2 + 1] >> s_shift);

        /* 先累计滤波前的值，才能报告"直流有多大" */
        s_raw_sum += l; s_raw_n++;

        if (left)  left[i]  = apply_dc_block(&s_dc[0], l);
        if (right) right[i] = apply_dc_block(&s_dc[1], r);
    }
    return frames;
}

static void raw_reset(void) { s_raw_sum = 0; s_raw_n = 0; }
static double raw_dc(void)  { return s_raw_n ? (double)s_raw_sum / s_raw_n : 0.0; }

/* ------------------------------------------------------------------ */
/* 单声道统计                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    int16_t vmin, vmax;
    int64_t sum, sum_sq;
    int     quiet;
} chan_stats_t;

static void stats_init(chan_stats_t *s)
{
    s->vmin = INT16_MAX; s->vmax = INT16_MIN;
    s->sum = 0; s->sum_sq = 0; s->quiet = 0;
}

static void stats_add(chan_stats_t *s, int16_t v)
{
    if (v < s->vmin) s->vmin = v;
    if (v > s->vmax) s->vmax = v;
    s->sum += v;
    s->sum_sq += (int64_t)v * v;
    if (v > -100 && v < 100) s->quiet++;
}

static double stats_rms(const chan_stats_t *s, int n)
{
    return n > 0 ? sqrt((double)s->sum_sq / n) : 0.0;
}

static void stats_print(const char *name, const chan_stats_t *s, int n, bool active)
{
    double rms = stats_rms(s, n);
    double dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -999.0;
    printf("%-6s %s: 范围 [%6d, %6d]  峰峰 %5d  RMS %7.1f (%6.1f dBFS)  静音 %5.1f%%\n",
           name, active ? "←" : " ", s->vmin, s->vmax, s->vmax - s->vmin,
           rms, dbfs, n > 0 ? 100.0 * s->quiet / n : 0.0);
}

/* ------------------------------------------------------------------ */
/* rec —— 采集并打印统计                                               */
/* ------------------------------------------------------------------ */
static void cmd_rec(int seconds)
{
    const int total = SAMPLE_RATE * seconds;
    int16_t lbuf[FRAMES_PER_READ], rbuf[FRAMES_PER_READ];
    chan_stats_t L, R;
    stats_init(&L);
    stats_init(&R);
    raw_reset();

    printf("\n>>> 采集 %d 秒（现在对着麦克风说话）...\n", seconds);
    fflush(stdout);

    /* 预热：丢掉前 125ms。直流阻断器要时间收敛，否则起始瞬态会变成一个
     * 400+ 的假峰值，把 max / 峰峰值 / 静音比全部带偏（实测踩过：
     * 静音采集报出 峰峰 589，其中 470 是瞬态而非信号）。 */
    const int warm = (SAMPLE_RATE / 8 + FRAMES_PER_READ - 1) / FRAMES_PER_READ;
    for (int w = 0; w < warm; w++) {
        read_block(lbuf, rbuf, FRAMES_PER_READ);
    }

    stats_init(&L);
    stats_init(&R);
    raw_reset();

    int done = 0;
    int64_t t0 = esp_log_timestamp();
    while (done < total) {
        int n = read_block(lbuf, rbuf, FRAMES_PER_READ);
        if (n < 0) {
            printf("!! I²S 读取失败，中止\n");
            fflush(stdout);
            return;
        }
        for (int i = 0; i < n; i++) {
            stats_add(&L, lbuf[i]);
            stats_add(&R, rbuf[i]);
        }
        done += n;
    }
    int64_t ms = esp_log_timestamp() - t0;

    /* A = 当前启用的那一路，B = 另一路 */
    const chan_stats_t *A = s_chan ? &R : &L;
    const chan_stats_t *B = s_chan ? &L : &R;
    int16_t amin = A->vmin, amax = A->vmax;
    double arms = stats_rms(A, done);
    double adbfs = arms > 0 ? 20.0 * log10(arms / 32768.0) : -999.0;
    double aquiet = 100.0 * A->quiet / done;
    int a_span = amax - amin, b_span = B->vmax - B->vmin;

    printf("\n================ 采集统计 ================\n");
    printf("样本数   : %d  (耗时 %" PRId64 " ms, 期望 %d ms)\n",
           done, ms, seconds * 1000);
    stats_print("左声道", &L, done, s_chan == 0);
    stats_print("右声道", &R, done, s_chan == 1);
    printf("当前使用 : %s声道\n", s_chan ? "右" : "左");
    printf("原始直流 : %.1f  (滤波前, 已由直流阻断滤除)\n", raw_dc());
    printf("直流阻断 : %s\n", s_dc_block ? "开" : "关");
    printf("RMS 电平 : %.1f dBFS  (已去直流)\n", adbfs);
    printf("==========================================\n");

    /* 自动判读。顺序有讲究：先排除"另一路有信号"这个最容易误判成
     * "没数据"的情况，再依次查更大的坑。 */
    printf("\n判读: ");
    if (a_span < 200 && b_span >= 200) {
        printf("❌ 数据在【%s声道】，当前取的却是【%s声道】—— 声道选错了！\n",
               s_chan ? "左" : "右", s_chan ? "右" : "左");
        printf("      软件修: 敲 `chan %d`\n", s_chan ? 0 : 1);
        printf("      硬件修: 模块 PS 跳线焊盘选错（见 docs/modules/93545.pdf）\n");
    } else if (a_span == 0 && b_span == 0) {
        printf("❌ 两路恒为 0 —— 完全没收到数据\n");
        printf("      依次查: 1) SD 是否接到 GPIO22  2) VDD 是否 3.3V\n");
        printf("              3) 板子是否真在跑这个固件  4) SCK 是否接到 GPIO26\n");
    } else if (a_span < 200) {
        printf("❌ 幅度极小（接近噪声底）\n");
        printf("      试: `shift 8`（位移方向错）或 `chan %d`（换一路）\n", s_chan ? 0 : 1);
    } else if (aquiet < 1.0 && (amax > 32000 || amin < -32000)) {
        printf("❌ 满量程噪声 —— 位移过头或时钟太快。试 `shift 24`\n");
    } else if (aquiet > 99.0) {
        printf("⚠️  几乎全程静音 —— 通道通了但没收到声音\n");
        printf("      查: 1) WS/SCK 是否接反  2) 麦克风进音孔是否被挡住/贴板\n");
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
        int n = read_block(buf, NULL, FRAMES_PER_READ);
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
/* level —— 实时电平表                                                 */
/*                                                                     */
/* 为什么需要它：rec 是一次性快照，要求人"正好在这几秒里说话"，时序上    */
/* 一错就得到假阴性（实测踩过：说话测试的 RMS 比静音还低，因为没对上）。 */
/* 电平表让人边说话边看数字抖，没有配合问题，是验证麦克风最直接的手段。 */
/* ------------------------------------------------------------------ */
static void cmd_level(int seconds)
{
    int16_t lbuf[FRAMES_PER_READ], rbuf[FRAMES_PER_READ];
    const int blocks   = seconds * SAMPLE_RATE / FRAMES_PER_READ;
    const int per_line = 6;                  /* 6 × 16ms ≈ 96ms 刷新一次 */

    printf("\n>>> 电平表 %d 秒 —— 现在开始说话，盯着数字和条子\n", seconds);
    printf("    条子高度跟着音量走 = 麦克风正常\n");
    printf("    一直贴在底部不动   = 没收到声音\n\n");

    int nb = 0;
    for (int b = 0; b < blocks; b++) {
        int n = read_block(lbuf, rbuf, FRAMES_PER_READ);
        if (n < 0) break;
        if (++nb % per_line) continue;

        int64_t sq = 0;
        int peak = 0;
        for (int i = 0; i < n; i++) {
            sq += (int64_t)lbuf[i] * lbuf[i];
            int a = lbuf[i] < 0 ? -lbuf[i] : lbuf[i];
            if (a > peak) peak = a;
        }
        double rms  = sqrt((double)sq / n);
        double dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -99.0;

        int bar = (int)((dbfs + 80.0) / 80.0 * 40.0);   /* -80..0 dBFS → 0..40 */
        if (bar < 0) bar = 0;
        if (bar > 40) bar = 40;

        char graph[41];
        memset(graph, '.', 40);
        memset(graph, '#', bar);
        graph[40] = '\0';

        printf("\r%7.1f dBFS |%s| 峰 %5d ", dbfs, graph, peak);
        fflush(stdout);
    }
    printf("\n\n完成。\n");
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
    printf("取用声道   : %s声道 (chan %d)\n", s_chan ? "右" : "左", s_chan);
    printf("直流阻断   : %s\n", s_dc_block ? "开" : "关");
    printf("  ↑ 模块 PS 跳线决定数据出在哪个声道, 默认左声道 (见 docs/modules/93545.pdf)\n");
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
    printf("  level [秒]  实时电平表, 边说话边看   (默认 10 秒)  ← 验证麦克风先试这个\n");
    printf("  rec [秒]    采集并打印统计量          (默认 3 秒)\n");
    printf("  wav [秒]    裸流输出 PCM，PC 侧存 WAV (默认 5 秒)\n");
    printf("  shift [n]   改 24→16bit 右移位数      (默认 16)\n");
    printf("  chan [0|1]  取左/右声道               (默认 0=左)\n");
    printf("  dc [0|1]    直流阻断开关              (默认 1=开)\n");
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

        if (strcmp(cmd, "level") == 0) {
            cmd_level(arg > 0 ? arg : 10);
        } else if (strcmp(cmd, "rec") == 0) {
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
        } else if (strcmp(cmd, "chan") == 0) {
            if (arg == 0 || arg == 1) {
                s_chan = arg;
                printf("取用声道 → %s声道\n", s_chan ? "右" : "左");
            } else {
                printf("chan 只能取 0(左) 或 1(右)\n");
            }
            fflush(stdout);
        } else if (strcmp(cmd, "dc") == 0) {
            s_dc_block = (arg != 0);
            printf("直流阻断 → %s\n", s_dc_block ? "开" : "关");
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
