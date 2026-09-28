#include "session.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "audio_frame.h"
#include "i2s_mic.h"
#include "transport.h"

static bool     s_recording;
static uint32_t s_seq;
static int64_t  s_t0_us;
static uint64_t s_samples_sent;

void session_start(void)
{
    /* 顺序有讲究：先重置直流阻断状态再预热，避免上一段的残留状态污染开头 */
    i2s_mic_reset_dc();
    i2s_mic_warmup();

    transport_reset_counters();
    s_seq = 0;
    s_samples_sent = 0;
    s_t0_us = esp_timer_get_time();
    s_recording = true;

    transport_send_text(FRAME_TYPE_STATUS, "RECORDING");
}

void session_stop(void)
{
    s_recording = false;

    /* 用「应发样本数 vs 实发样本数」判断有没有丢音频。
     *
     * ⚠️ 这是唯一能反映背压丢帧的指标。**帧序号连续并不代表 I²S 没丢样本** ——
     *    序号是固件自己生成的，I²S 漏采的样本根本不会变成帧。
     *    只有"按时间应该产出多少 vs 实际发出多少"的差值能暴露它。
     *
     * 允许一个小容差：停止时最后一两个 DMA 缓冲可能还在路上。 */
    const int64_t elapsed_us = esp_timer_get_time() - s_t0_us;
    const uint64_t expected = (uint64_t)elapsed_us * I2S_MIC_SAMPLE_RATE / 1000000ULL;
    const int64_t deficit = (int64_t)expected - (int64_t)s_samples_sent;

    const uint32_t tol = SESSION_CHUNK_SAMPLES * 2;   /* ~32ms */
    char msg[160];
    snprintf(msg, sizeof(msg),
             "STOPPED dur_ms=%lld samples=%llu expected=%llu deficit=%lld "
             "tx_block_ms=%u %s",
             (long long)(elapsed_us / 1000),
             (unsigned long long)s_samples_sent,
             (unsigned long long)expected,
             (long long)deficit,
             (unsigned)transport_tx_blocked_ms(),
             (deficit > (int64_t)tol) ? "LOST_DATA" : "OK");

    transport_send_text(FRAME_TYPE_STATUS, msg);
}

bool session_is_recording(void) { return s_recording; }

void session_tick(void)
{
    if (!s_recording) return;

    static int16_t pcm[SESSION_CHUNK_SAMPLES];
    static uint8_t payload[8 + SESSION_CHUNK_SAMPLES * sizeof(int16_t)];

    const int n = i2s_mic_read(pcm, SESSION_CHUNK_SAMPLES);
    if (n <= 0) return;

    const uint32_t ts_ms = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
    const size_t plen = audio_payload_pack(payload, s_seq++, ts_ms, pcm, (size_t)n);

    transport_send(FRAME_TYPE_AUDIO, payload, (uint16_t)plen);
    s_samples_sent += (uint64_t)n;
}
