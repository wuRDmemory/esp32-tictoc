/*
 * AFE 能力抽象：声学前端（Acoustic Front End）。
 *
 * ⚠️ 这是一层**能力抽象**，不是"AFE 驱动" —— 因为两块板的能力根本不同：
 *
 *   BOARD_HAS_ESPSR = 0（经典 ESP32 / PICO）
 *       → afe_passthrough.c 生效。全部空操作，**永远不上报事件**。
 *         「聆听结束」仍由 PC 侧的 RMS 静音检测决定 —— **行为与引入本层之前一模一样**。
 *
 *   BOARD_HAS_ESPSR = 1（ESP32-S3 / S3-CAM）
 *       → afe_espsr.c 生效。用乐鑫 ESP-SR 的 AFE + VAD 做**板端判停**（decisions.md D13）。
 *
 * **为什么需要这层**：D13 要求把判停前移到板端，但**只有 S3 做得到**
 * （经典 ESP32 没有 AFE，见 docs/wakeword-research.md §2）。有了它，
 * session.c 两边共用一份代码，不必写 #if。
 *
 * ⚠️ **音频路径不受影响**：AFE **只旁观**，发给 PC 的仍是原始 PCM（D13.4）。
 *    理由见 decisions.md —— FunASR 自带 VAD 与降噪，喂处理过的音频反而可能
 *    引入它不认识的失真。**所以本层没有"取处理后音频"的接口，这是故意的。**
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/* VAD 事件。**消费性**：取走即清除。 */
typedef enum {
    AFE_EVENT_NONE = 0,
    AFE_EVENT_SPEECH_START,   /* 检测到开始说话 */
    AFE_EVENT_SPEECH_END,     /* 说话结束（连续静音超过门限）← D13 要的就是这个 */
} afe_event_t;

/* 本板 AFE 实现的名字，供 diag 显示（"passthrough" / "esp-sr"）。
 * 让"以为在用板端判停"成为可见的事实，而不是幻觉。 */
const char *afe_name(void);

/* 本板**是否具备**板端判停能力。
 *
 * ⚠️ 这是**能力查询**，不是开关 —— 想关掉板端判停请改 board.h / sdkconfig，
 *    不要在这里返回 false。理由见项目 CLAUDE.md「不要自己发明」。 */
bool afe_available(void);

/* 初始化（幂等，可重复调用）。passthrough 恒返回 ESP_OK。
 * 必须在 i2s_mic_init() 之后、session_start() 之前调用一次。 */
esp_err_t afe_init(void);

/* 喂一段 16bit 单声道 PCM —— 就是 i2s_mic_read() 的输出。
 *
 * n **不要求**等于 AFE 的帧长：实现内部负责攒帧
 * （我们每次 256 样本/16ms，而 AFE 通常要 512 样本/32ms）。 */
void afe_feed(const int16_t *pcm, int n);

/* 取一个待处理事件（消费性）。无事件返回 AFE_EVENT_NONE。
 *
 * ⚠️ **必须每轮循环都调用**，否则事件会在下一次 feed 时被覆盖而丢失。 */
afe_event_t afe_poll_event(void);

/* 重置 VAD 状态。**每次开始录音前必须调用**：
 * 上一段录音结束时的残留状态（例如停在 SILENCE）会污染新一段的开头。 */
void afe_reset(void);

/* 诊断用：当前 VAD 状态的可读字符串（"speech" / "silence" / "n/a"）。
 * 供 `vad` 命令做门限标定（prd.md §12-Q9）。 */
const char *afe_vad_state_str(void);
