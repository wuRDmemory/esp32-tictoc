/*
 * ICS-43434 采音实现。
 *
 * 本文件的取音逻辑是从 mic_test.c **原样搬移**的 —— 它的每一个参数都由
 * docs/hardware.md §7 的实测确定。搬移时没有改动任何数值，只把 static
 * 去掉并补上对外接口。**改这里之前先读那一节。**
 */
#include "i2s_mic.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

/* 接线 —— 引脚号在 board.h 里按板定义（两块板子没有一个是相同的）。
 * LR 引脚(2) 接 GND → 数据出在左声道；VDD(5) 接 3V3 + 0.1µF；SD 加 100kΩ 下拉 */

#define BYTES_PER_READ  (I2S_MIC_FRAMES_PER_READ * 2 * 4)   /* 2 槽 × 4 字节 */

static const char *TAG = "i2s_mic";

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
 * 364~424 且逐次漂移），不除掉会让 RMS、静音比这些统计量全部失真。
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

        /* 两路都滤波（保持状态热），只由调用方挑选用哪一路 */
        if (left)  left[i]  = apply_dc_block(&s_dc[0], l);
        if (right) right[i] = apply_dc_block(&s_dc[1], r);
    }
    return frames;
}

/* ------------------------------------------------------------------ */
/* I²S 初始化                                                          */
/* ------------------------------------------------------------------ */
static esp_err_t i2s_init_once(bool use_preferred_clk)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = I2S_MIC_FRAMES_PER_READ;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_STEREO),   /* ⚠️ 必须 STEREO：64 SCK/帧 */
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,             /* ICS-43434 不需要 MCLK */
            .bclk = (gpio_num_t)BOARD_I2S_BCLK,
            .ws   = (gpio_num_t)BOARD_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)BOARD_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (use_preferred_clk) {
        /* 板级首选时钟源：经典 ESP32 是 APLL（更精确），
         * S3 没有 APLL、用 PLL_160M。见 board.h */
        std_cfg.clk_cfg.clk_src = BOARD_I2S_CLK_SRC;
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
    s_clk_name = use_preferred_clk ? BOARD_I2S_CLK_SRC_NAME : "DEFAULT";
    return ESP_OK;
}

esp_err_t i2s_mic_init(void)
{
    /* 先用板级首选时钟源，失败则回退到 DEFAULT。
     * 并把【实际生效的】是哪个打出来 —— 别让"以为用了 APLL"成为幻觉 */
    esp_err_t err = i2s_init_once(true);   /* 先用板级首选 */
    if (err == ESP_OK) return ESP_OK;

    ESP_LOGW(TAG, "%s 时钟源初始化失败 (%s)，回退到 DEFAULT",
             BOARD_I2S_CLK_SRC_NAME, esp_err_to_name(err));
    err = i2s_init_once(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I²S 初始化彻底失败: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "检查：1) GPIO %d/%d/%d 是否被别的东西占用", 
                 BOARD_I2S_BCLK, BOARD_I2S_WS, BOARD_I2S_DIN);
        ESP_LOGE(TAG, "      2) 板子选择对不对（menuconfig → Target board）");
    }
    return err;
}

const char *i2s_mic_clk_name(void) { return s_clk_name; }

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
int i2s_mic_read(int16_t *out, int max_samples)
{
    if (max_samples > I2S_MIC_FRAMES_PER_READ) max_samples = I2S_MIC_FRAMES_PER_READ;

    int16_t l[I2S_MIC_FRAMES_PER_READ];
    int16_t r[I2S_MIC_FRAMES_PER_READ];
    int n = read_block(l, r, max_samples);
    if (n <= 0) return n;

    const int16_t *src = s_chan ? r : l;
    memcpy(out, src, (size_t)n * sizeof(int16_t));
    return n;
}

int i2s_mic_read_both(int16_t *left, int16_t *right, int max_samples)
{
    if (max_samples > I2S_MIC_FRAMES_PER_READ) max_samples = I2S_MIC_FRAMES_PER_READ;
    return read_block(left, right, max_samples);
}

int i2s_mic_read_raw(int32_t *left, int32_t *right, int max_frames)
{
    static uint8_t raw[BYTES_PER_READ];
    size_t got = 0;

    esp_err_t err = i2s_channel_read(s_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(1000));
    if (err != ESP_OK || got == 0) return -1;

    int frames = got / 8;
    if (frames > max_frames) frames = max_frames;

    const int32_t *w = (const int32_t *)raw;
    for (int i = 0; i < frames; i++) {
        if (left)  left[i]  = w[i * 2];
        if (right) right[i] = w[i * 2 + 1];
    }
    return frames;
}

void i2s_mic_set_shift(int n) { s_shift = n; }
int  i2s_mic_get_shift(void)  { return s_shift; }

void i2s_mic_set_chan(int n)  { s_chan = n; }
int  i2s_mic_get_chan(void)   { return s_chan; }

void i2s_mic_set_dc_block(bool on) { s_dc_block = on; }
bool i2s_mic_get_dc_block(void)    { return s_dc_block; }

void i2s_mic_reset_dc(void)
{
    s_dc[0].x1 = s_dc[0].y1 = 0.0f;
    s_dc[1].x1 = s_dc[1].y1 = 0.0f;
}

void i2s_mic_warmup(void)
{
    int16_t tmp[I2S_MIC_FRAMES_PER_READ];
    /* 125ms。直流阻断器收敛需约 1000 个样本，不丢的话起始瞬态会变成一个
     * 400+ 的假峰值（实测踩过：静音采集报出峰峰 589，其中 470 是瞬态） */
    const int blocks = (I2S_MIC_SAMPLE_RATE / 8 + I2S_MIC_FRAMES_PER_READ - 1)
                       / I2S_MIC_FRAMES_PER_READ;
    for (int i = 0; i < blocks; i++) {
        i2s_mic_read(tmp, I2S_MIC_FRAMES_PER_READ);
    }
}

void   i2s_mic_raw_reset(void) { s_raw_sum = 0; s_raw_n = 0; }
double i2s_mic_raw_dc(void)
{
    return s_raw_n ? (double)s_raw_sum / s_raw_n : 0.0;
}
