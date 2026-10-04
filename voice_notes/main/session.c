#include "session.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "afe.h"
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

    /* 同理重置 VAD：上一段结束时的残留状态（通常停在 SILENCE）
     * 会让新一段开头的"开始说话"识别不出来 */
    afe_reset();

    transport_reset_counters();
    s_seq = 0;
    s_samples_sent = 0;
    s_t0_us = esp_timer_get_time();
    s_recording = true;

    transport_send_text(FRAME_TYPE_STATUS, "RECORDING");
}

/* by 说明是谁发起的停止：命令/按键是 "CMD"，板端 VAD 判停是 "VAD"。
 * PC 侧据此区分"我让它停的"和"它自己停的"（decisions.md D13.3）。 */
static void session_stop_by(const char *by)
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
             "STOPPED by=%s dur_ms=%lld samples=%llu expected=%llu deficit=%lld "
             "tx_block_ms=%u %s",
             by,
             (long long)(elapsed_us / 1000),
             (unsigned long long)s_samples_sent,
             (unsigned long long)expected,
             (long long)deficit,
             (unsigned)transport_tx_blocked_ms(),
             (deficit > (int64_t)tol) ? "LOST_DATA" : "OK");

    transport_send_text(FRAME_TYPE_STATUS, msg);
}

void session_stop(void) { session_stop_by("CMD"); }

bool session_is_recording(void) { return s_recording; }

void session_tick(void)
{
    if (!s_recording) return;

    static int16_t pcm[SESSION_CHUNK_SAMPLES];
    static uint8_t payload[8 + SESSION_CHUNK_SAMPLES * sizeof(int16_t)];

    const int n = i2s_mic_read(pcm, SESSION_CHUNK_SAMPLES);
    if (n <= 0) return;

    /* ---- 板端判停（decisions.md D13）：AFE 只旁观 ----
     *
     * ⚠️ 这里的先后顺序是刻意的：「先喂 → 再看 → 照常发 → 最后停」
     *   - AFE 只读 pcm 做判断，**不改动下面的音频路径**（D13.4）
     *   - 触发判停的这一块**照常发出去**，不丢样本（它已是静音，发了无害）
     *   - 停止放在发送**之后**，所以最后一段音频永远完整，不会缺尾巴
     *
     * PICO 上 afe_poll_event() 恒为 NONE —— 这段完全等价于不存在，行为不变。 */
    afe_feed(pcm, n);
    const bool speech_end = (afe_poll_event() == AFE_EVENT_SPEECH_END);

    const uint32_t ts_ms = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
    const size_t plen = audio_payload_pack(payload, s_seq++, ts_ms, pcm, (size_t)n);

    transport_send(FRAME_TYPE_AUDIO, payload, (uint16_t)plen);
    s_samples_sent += (uint64_t)n;

    if (speech_end) {
        session_stop_by("VAD");
    }
}
