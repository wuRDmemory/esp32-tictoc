/*
 * 诊断命令集 —— 从阶段 1 的 mic_test.c 演化而来。
 *
 * ⚠️ **这些命令不能删。** 阶段 1 的排查几乎全靠它们（`raw` 定位比特对齐、
 *    `level` 验证麦克风响应、`rec` 的双声道判读区分"没数据"和"选错声道"）。
 *    删掉等于把下次排查的工具扔掉。
 *
 * 由 app_main.c 通过 diag_handle_command() 调用。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "afe.h"
#include "i2s_mic.h"
#include "transport.h"


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
    const int total = I2S_MIC_SAMPLE_RATE * seconds;
    int16_t lbuf[I2S_MIC_FRAMES_PER_READ], rbuf[I2S_MIC_FRAMES_PER_READ];
    chan_stats_t L, R;

    printf("\n>>> 采集 %d 秒（现在对着麦克风说话）...\n", seconds);
    fflush(stdout);

    /* 预热：丢掉前 125ms。直流阻断器要时间收敛，否则起始瞬态会变成一个
     * 400+ 的假峰值，把 max / 峰峰值 / 静音比全部带偏（实测踩过：
     * 静音采集报出 峰峰 589，其中 470 是瞬态而非信号）。 */
    const int warm = (I2S_MIC_SAMPLE_RATE / 8 + I2S_MIC_FRAMES_PER_READ - 1) / I2S_MIC_FRAMES_PER_READ;
    for (int w = 0; w < warm; w++) {
        i2s_mic_read_both(lbuf, rbuf, I2S_MIC_FRAMES_PER_READ);
    }

    stats_init(&L);
    stats_init(&R);
    i2s_mic_raw_reset();

    int done = 0;
    int64_t t0 = esp_log_timestamp();
    while (done < total) {
        int n = i2s_mic_read_both(lbuf, rbuf, I2S_MIC_FRAMES_PER_READ);
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
    const chan_stats_t *A = i2s_mic_get_chan() ? &R : &L;
    const chan_stats_t *B = i2s_mic_get_chan() ? &L : &R;
    int16_t amin = A->vmin, amax = A->vmax;
    double arms = stats_rms(A, done);
    double adbfs = arms > 0 ? 20.0 * log10(arms / 32768.0) : -999.0;
    double aquiet = 100.0 * A->quiet / done;
    int a_span = amax - amin, b_span = B->vmax - B->vmin;

    printf("\n================ 采集统计 ================\n");
    printf("样本数   : %d  (耗时 %" PRId64 " ms, 期望 %d ms)\n",
           done, ms, seconds * 1000);
    stats_print("左声道", &L, done, i2s_mic_get_chan() == 0);
    stats_print("右声道", &R, done, i2s_mic_get_chan() == 1);
    printf("当前使用 : %s声道\n", i2s_mic_get_chan() ? "右" : "左");
    printf("原始直流 : %.1f  (滤波前, 已由直流阻断滤除)\n", i2s_mic_raw_dc());
    printf("直流阻断 : %s\n", i2s_mic_get_dc_block() ? "开" : "关");
    printf("RMS 电平 : %.1f dBFS  (已去直流)\n", adbfs);
    printf("==========================================\n");

    /* 自动判读。顺序有讲究：先排除"另一路有信号"这个最容易误判成
     * "没数据"的情况，再依次查更大的坑。 */
    printf("\n判读: ");
    if (a_span < 200 && b_span >= 200) {
        printf("❌ 数据在【%s声道】，当前取的却是【%s声道】—— 声道选错了！\n",
               i2s_mic_get_chan() ? "左" : "右", i2s_mic_get_chan() ? "右" : "左");
        printf("      软件修: 敲 `chan %d`\n", i2s_mic_get_chan() ? 0 : 1);
        printf("      硬件修: 模块 PS 跳线焊盘选错（见 docs/modules/93545.pdf）\n");
    } else if (a_span == 0 && b_span == 0) {
        printf("❌ 两路恒为 0 —— 完全没收到数据\n");
        /* ⚠️ 引脚号必须来自 board.h —— 写死会在换板后指错脚，
         *    而这种"诊断信息本身是错的"最难发现（实测踩过） */
        printf("      依次查: 1) 麦克风 SD 是否接到 GPIO%d\n", BOARD_I2S_DIN);
        printf("              2) 麦克风 VDD 是否接 3V3（不是 5V！）且已上电\n");
        printf("              3) SCK 是否接到 GPIO%d，WS 是否接到 GPIO%d\n",
               BOARD_I2S_BCLK, BOARD_I2S_WS);
        printf("              4) 板子是否真在跑这个固件（看板名：%s）\n", BOARD_NAME);
    } else if (a_span < 200) {
        printf("❌ 幅度极小（接近噪声底）\n");
        printf("      试: `shift 8`（位移方向错）或 `chan %d`（换一路）\n", i2s_mic_get_chan() ? 0 : 1);
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
    const int total = I2S_MIC_SAMPLE_RATE * seconds;
    int done = 0;
    int16_t buf[I2S_MIC_FRAMES_PER_READ];

    /* 绕过 stdio：直接写 UART，避免 \n → \r\n 转换污染二进制数据 */
    transport_wait_tx_done();
    char hdr[64];
    int hlen = snprintf(hdr, sizeof(hdr), "WAV_BEGIN %d %d\n", total, I2S_MIC_SAMPLE_RATE);
    transport_write_raw((const uint8_t *)hdr, (size_t)hlen);
    transport_wait_tx_done();

    while (done < total) {
        int n = i2s_mic_read(buf, I2S_MIC_FRAMES_PER_READ);
        if (n < 0) break;
        transport_write_raw((const uint8_t *)buf, (size_t)n * 2);
        done += n;
    }
    transport_wait_tx_done();
    transport_write_raw((const uint8_t *)"\nWAV_END\n", 9);

    printf("已发送 %d 个样本（%d 字节 PCM）\n", done, done * 2);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* raw —— 打印未经位移的 32bit I²S 原始字                              */
/*                                                                     */
/* 用途：一次性判定"24bit 数据到底落在 32bit 字的哪几位"。             */
/* 判据：看哪个字节恒为 0 ——                                          */
/*   低字节恒 0  → 数据在 [31:8]，标准 I²S，即当前假设                 */
/*   高字节恒 0  → 数据右对齐在 [23:0]，不是标准 I²S                   */
/*   都不为 0    → 对齐位置不同，据此调整 shift                        */
/*                                                                     */
/* 为什么需要它：如果只是"声音偏小"，光看 dBFS 分不出是"麦克风没收到"  */
/* 还是"收到了但位移取错了几位"。原始 32bit 是唯一能定案的证据。        */
/* ------------------------------------------------------------------ */
static void cmd_raw(int n)
{
    printf("\n>>> 原始 32bit I²S 字（前 %d 个左声道样本）\n", n);
    printf("    看哪个字节恒为 0 即可判定位对齐：\n");
    printf("      低字节恒 0 → 数据在 [31:8]（标准 I²S，当前假设）\n");
    printf("      高字节恒 0 → 数据右对齐在 [23:0]（非标准 I²S）\n\n");
    printf("    #   左声道 raw32       右声道 raw32      >>%d 后\n", i2s_mic_get_shift());
    printf("  ---- ------------------ ------------------ ----------\n");

    int32_t lw[I2S_MIC_FRAMES_PER_READ], rw[I2S_MIC_FRAMES_PER_READ];
    int shown = 0;
    while (shown < n) {
        int frames = i2s_mic_read_raw(lw, rw, I2S_MIC_FRAMES_PER_READ);
        if (frames <= 0) break;
        for (int i = 0; i < frames && shown < n; i++, shown++) {
            printf("  %4d 0x%08" PRIX32 "       0x%08" PRIX32 "     %8d\n",
                   shown, (uint32_t)lw[i], (uint32_t)rw[i],
                   (int16_t)(lw[i] >> i2s_mic_get_shift()));
        }
    }
    printf("\n");
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
    int16_t lbuf[I2S_MIC_FRAMES_PER_READ], rbuf[I2S_MIC_FRAMES_PER_READ];
    const int blocks = seconds * I2S_MIC_SAMPLE_RATE / I2S_MIC_FRAMES_PER_READ;
    /* 每 ~500ms 打一行。之前用 \r 原地刷新，导致用户粘贴出来只剩最后一行，
     * 中间的变化全被覆盖 —— 那是工具的观测缺陷，不是数据的问题。 */
    const int per_line = I2S_MIC_SAMPLE_RATE / 2 / I2S_MIC_FRAMES_PER_READ;

    printf("\n>>> 电平表 %d 秒 —— 现在开始说话（或拍手/吹气）\n", seconds);
    printf("    条子跟着音量走 = 麦克风正常；一直贴底 = 没收到声音\n");
    printf("    每行 0.5 秒，最后一行是全程峰值保持\n\n");

    int nb = 0, hold = 0;
    for (int b = 0; b < blocks; b++) {
        int n = i2s_mic_read_both(lbuf, rbuf, I2S_MIC_FRAMES_PER_READ);
        if (n < 0) break;
        if (++nb % per_line) continue;

        int64_t sq = 0;
        int peak = 0;
        for (int i = 0; i < n; i++) {
            sq += (int64_t)lbuf[i] * lbuf[i];
            int a = lbuf[i] < 0 ? -lbuf[i] : lbuf[i];
            if (a > peak) peak = a;
        }
        if (peak > hold) hold = peak;

        double rms  = sqrt((double)sq / n);
        double dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -99.0;

        int bar = (int)((dbfs + 80.0) / 80.0 * 40.0);   /* -80..0 dBFS → 0..40 */
        if (bar < 0) bar = 0;
        if (bar > 40) bar = 40;

        char graph[41];
        memset(graph, '.', 40);
        memset(graph, '#', bar);
        graph[40] = '\0';

        printf("%6.1f dBFS |%s| 峰 %6d   (峰值保持 %d)\n", dbfs, graph, peak, hold);
        fflush(stdout);
    }

    printf("\n================ 电平表结论 ================\n");
    printf("全程最大峰值: %d  (%.1f dBFS)\n", hold,
           hold > 0 ? 20.0 * log10(hold / 32768.0) : -999.0);
    printf("参考: 正常说话(30cm) 应达到 -45 dBFS 左右, 即峰值约 1800\n");
    if (hold < 300) {
        printf("判读: ❌ 全程没超过 %d —— 麦克风没有收到任何声音\n", hold);
        printf("      下一步: 敲 `raw 16` 看原始位对齐, 再试 `shift 8` / `chan 1` / `dc 0`\n");
    } else if (hold < 1800) {
        printf("判读: ⚠️  有信号但偏弱 —— 可能是距离远/声音小, 或位移少取了几位\n");
        printf("      下一步: 贴近麦克风再试一次; 仍偏弱就敲 `raw 16` 定位\n");
    } else {
        printf("判读: ✅ 麦克风工作正常\n");
    }
    printf("============================================\n\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* VAD 标定 —— prd.md §12-Q9 说"静音时长必须在 S3 上实测标定"，
 * 这个命令就是那把尺子。没有它，Q9 只能靠猜。
 *
 * ⚠️ 只在有 AFE 的板上才有意义（PICO 会直接说明原因并退出，不是静默失败）。 */
static void cmd_vad(int seconds)
{
    if (!afe_available()) {
        /* 两种原因必须分开说 —— 否则在 S3 上会打印"经典 ESP32 不支持 AFE"这种
         * 与事实不符的话，把"还没接入"误导成"这块板做不到"。 */
        printf("\n>>> 板端判停当前不可用（AFE 实现：%s）\n", afe_name());
#if BOARD_HAS_ESPSR
        printf("    本板是 S3，**具备 AFE 能力**，但 **AFE 初始化失败**\n");
        printf("    → 往上翻 esp_afe_espsr 的日志（afe_config_print 会打出真实生效值）\n");
        printf("    → 常见原因：input_format=\"M\" 不被接受 / PSRAM 没开 / 内存不足\n");
#else
        printf("    本板是经典 ESP32，**硬件不支持 AFE**（docs/wakeword-research.md §2）\n");
        printf("    → 这是能力差异，不是故障。换 S3 再跑本命令\n");
#endif
        printf("    「聆听结束」仍由 PC 侧 RMS 静音检测决定（prd.md FR-5）\n\n");
        fflush(stdout);
        return;
    }

    printf("\n>>> VAD 标定 %d 秒 —— 说一句话，然后停顿，看它多快判停\n", seconds);
    printf("    每行 = VAD 状态变化。★ SPEECH_END 就是「判停」\n");
    printf("    要标定的值：从「停止说话」到 ★ 之间隔了多久（= prd.md Q9 的静音时长）\n");
    printf("    建议依次试停顿 1 / 3 / 5 秒，看哪一次被误判成说完了\n\n");

    int16_t buf[I2S_MIC_FRAMES_PER_READ];
    const int blocks = seconds * I2S_MIC_SAMPLE_RATE / I2S_MIC_FRAMES_PER_READ;

    i2s_mic_reset_dc();
    i2s_mic_warmup();
    afe_reset();

    const int64_t t0 = esp_timer_get_time();
    const char *last = NULL;
    int n_end = 0;
    double n_speech_dbfs_sum = 0.0;   /* speech 期间的平均电平，供结论区判读 */
    int    n_speech_blocks = 0;

    /* 平方的指数平均，时间常数约 128ms（8 块 × 16ms）。
     * 用滑动值而不是单块值 —— 单块抖动太大，看不出"这一刻环境有多响"。 */
    double ema_msq = 0.0;

    for (int b = 0; b < blocks; b++) {
        const int n = i2s_mic_read(buf, I2S_MIC_FRAMES_PER_READ);
        if (n < 0) break;

        afe_feed(buf, n);

        int64_t sq = 0;
        for (int i = 0; i < n; i++) sq += (int64_t)buf[i] * buf[i];
        const double msq = (n > 0) ? (double)sq / n : 0.0;
        ema_msq = (ema_msq == 0.0) ? msq : (ema_msq * 0.875 + msq * 0.125);

        /* 先取事件再取状态：事件是消费性的，取走即清除 */
        const afe_event_t ev = afe_poll_event();
        const char *cur = afe_vad_state_str();
        const bool changed = (last == NULL) || (strcmp(cur, last) != 0);

        /* ⚠️ 电平是**判读误报的关键**：VAD 报 speech 时若电平只有底噪水平
         *    （实测本模块约 -60 dBFS），那就是能量门限撞上底噪，
         *    而不是真的有人说话 —— 两者修法完全不同。 */
        if (strcmp(cur, "speech") == 0 && ema_msq > 0.0) {
            n_speech_dbfs_sum += 10.0 * log10(ema_msq / (32768.0 * 32768.0));
            n_speech_blocks++;
        }

        if (ev != AFE_EVENT_NONE || changed) {
            const double rms  = sqrt(ema_msq);
            const double dbfs = rms > 0 ? 20.0 * log10(rms / 32768.0) : -99.0;

            printf("  t=%5.2fs  %-8s [电平%7.1f dBFS]",
                   (esp_timer_get_time() - t0) / 1e6, cur, dbfs);
            if (ev == AFE_EVENT_SPEECH_END) {
                printf("  ★ SPEECH_END ← 判停");
                n_end++;
            } else if (ev == AFE_EVENT_SPEECH_START) {
                printf("  (开始说话)");
            }
            printf("\n");
            fflush(stdout);
            last = cur;
        }
    }

    printf("\n================ VAD 标定结论 ================\n");

    /* 电平是判读误报的关键 —— 放在最前面 */
    if (n_speech_blocks > 0) {
        printf("VAD 判为 speech 时的平均电平: %.1f dBFS\n",
               n_speech_dbfs_sum / n_speech_blocks);
        printf("  判读: 接近安静底噪（本模块约 -60 dBFS）= 【能量门限撞上底噪】\n");
        printf("        → 调 vad_energy_threshold 即可，不必换 VAD 模型\n");
        printf("        明显高于底噪 = 当时【确实有声音】，那是正常检测\n");
    } else {
        printf("全程没有出现 speech —— 见下方\n");
    }

    if (n_end == 0) {
        printf("\n全程没有触发判停 —— 要么一直在说话，要么门限偏高\n");
        printf("下一步: 安静几秒再试；仍不触发就要调 VAD 门限/模式\n");
    } else {
        printf("\n触发判停 %d 次。请对照上面的时间戳，确认「停顿多久才判停」\n", n_end);
        printf("判读: 停顿 1 秒就被判停 = 门限太低（句子会被切断）\n");
        printf("      停顿 5 秒还没判停 = 门限太高（失去「快」的意义）\n");
        printf("      但若【没说话也报 speech】，先看上面的平均电平\n");
    }
    printf("============================================\n\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
void diag_print_cfg(void)
{
    printf("\n================ 当前配置 ================\n");
    printf("目标板     : %s  [%s]\n", BOARD_NAME, BOARD_CHIP_FAMILY);
    printf("采样率     : %d Hz\n", I2S_MIC_SAMPLE_RATE);
    printf("时钟源     : %s\n", i2s_mic_clk_name());
    printf("AFE/VAD    : %s  %s\n", afe_name(),
           afe_available() ? "← 板端判停可用（D13）"
                           : "（本板无 AFE，判停走 PC 侧 RMS）");
    printf("位宽/槽    : 32 bit × 2 槽 = 64 SCK/帧\n");
    printf("  ↑ ICS-43434 要求每 WS 帧正好 64 个 SCK（数据手册 C2）\n");
    printf("  ↑ 若用 I2S_SLOT_MODE_MONO 会变成 32 SCK/帧，麦克风直接不工作\n");
    printf("BCLK 期望  : %.3f MHz  (64 × %d)\n", 64.0 * I2S_MIC_SAMPLE_RATE / 1e6, I2S_MIC_SAMPLE_RATE);
    printf("WS   期望  : %.3f kHz\n", I2S_MIC_SAMPLE_RATE / 1000.0);
    printf("右移位数   : %d   (24bit 数据在 32bit 字的 [31:8])\n", i2s_mic_get_shift());
    printf("取用声道   : %s声道 (chan %d)\n", i2s_mic_get_chan() ? "右" : "左", i2s_mic_get_chan());
    printf("直流阻断   : %s\n", i2s_mic_get_dc_block() ? "开" : "关");
    printf("  ↑ 模块 PS 跳线决定数据出在哪个声道, 默认左声道 (见 docs/modules/93545.pdf)\n");
    printf("DMA        : 4 缓冲 × %d 帧 = %d ms 延迟\n",
           I2S_MIC_FRAMES_PER_READ, I2S_MIC_FRAMES_PER_READ * 1000 / I2S_MIC_SAMPLE_RATE * 4);
    printf("引脚       : BCLK=IO%d  WS=IO%d  DIN=IO%d\n",
           I2S_MIC_PIN_BCLK, I2S_MIC_PIN_WS, I2S_MIC_PIN_DIN);
    /* console 可能是 UART，也可能是 S3 的原生 USB —— 相关 Kconfig 符号
     * 只在对应选项下才存在，所以要条件编译。
     * 踩过的坑：写死 CONFIG_ESP_CONSOLE_UART_BAUDRATE 后，把 console 换成
     * USB-Serial-JTAG 时该符号消失，直接编译不过。 */
#if CONFIG_ESP_CONSOLE_UART
    printf("控制台     : UART%d @ %d 8N1\n",
           CONFIG_ESP_CONSOLE_UART_NUM, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    printf("控制台     : USB-Serial-JTAG（原生 USB，无波特率概念）\n");
#elif CONFIG_ESP_CONSOLE_USB_CDC
    printf("控制台     : USB-CDC（原生 USB）\n");
#else
    printf("控制台     : 其他\n");
#endif
    printf("==========================================\n\n");
    fflush(stdout);
}

void diag_print_help(void)
{
    printf("\n命令:\n");
    printf("  level [秒]  实时电平表, 边说话边看   (默认 10 秒)  ← 验证麦克风先试这个\n");
    printf("  raw [n]     打印原始 32bit 字, 判定位对齐 (默认 16)\n");
    printf("  rec [秒]    采集并打印统计量          (默认 3 秒)\n");
    printf("  wav [秒]    裸流输出 PCM，PC 侧存 WAV (默认 5 秒)\n");
    printf("  shift [n]   改 24→16bit 右移位数      (默认 16)\n");
    printf("  chan [0|1]  取左/右声道               (默认 0=左)\n");
    printf("  dc [0|1]    直流阻断开关              (默认 1=开)\n");
    printf("  vad [秒]    VAD 判停标定表 (默认 20 秒)  ← 只有 S3 有意义\n");
    printf("  cfg         重看当前配置\n");
    printf("  help        本帮助\n\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
void diag_handle_command(const char *line)
{
    if (line == NULL || line[0] == '\0') return;

    int arg = 0;
    char cmd[32] = {0};
    sscanf(line, "%31s %d", cmd, &arg);

    if (strcmp(cmd, "raw") == 0) {
        cmd_raw(arg > 0 ? arg : 16);
    } else if (strcmp(cmd, "level") == 0) {
        cmd_level(arg > 0 ? arg : 10);
    } else if (strcmp(cmd, "rec") == 0) {
        cmd_rec(arg > 0 ? arg : 3);
    } else if (strcmp(cmd, "wav") == 0) {
        cmd_wav(arg > 0 ? arg : 5);
    } else if (strcmp(cmd, "shift") == 0) {
        if (arg >= 0 && arg <= 31) {
            i2s_mic_set_shift(arg);
            printf("右移位数 → %d\n", i2s_mic_get_shift());
        } else {
            printf("shift 取值范围 0..31\n");
        }
        fflush(stdout);
    } else if (strcmp(cmd, "chan") == 0) {
        if (arg == 0 || arg == 1) {
            i2s_mic_set_chan(arg);
            printf("取用声道 → %s声道\n", i2s_mic_get_chan() ? "右" : "左");
        } else {
            printf("chan 只能取 0(左) 或 1(右)\n");
        }
        fflush(stdout);
    } else if (strcmp(cmd, "dc") == 0) {
        i2s_mic_set_dc_block(arg != 0);
        printf("直流阻断 → %s\n", i2s_mic_get_dc_block() ? "开" : "关");
        fflush(stdout);
    } else if (strcmp(cmd, "vad") == 0) {
        cmd_vad(arg > 0 ? arg : 20);
    } else if (strcmp(cmd, "cfg") == 0) {
        diag_print_cfg();
    } else if (strcmp(cmd, "help") == 0) {
        diag_print_help();
    } else {
        printf("未知命令: %s（敲 help 看用法）\n", cmd);
        fflush(stdout);
    }
}
