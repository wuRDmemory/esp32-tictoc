/*
 * AFE 的 ESP-SR 实现 —— 给 ESP32-S3 用（板端 VAD 判停，decisions.md D13）。
 *
 * ⚠️⚠️ **当前是接入 ESP-SR 之前的临时实现，尚未实现板端判停。**
 *
 *    它的唯一作用是**让 S3 的编译和链接通过**，避免把仓库留在
 *    "PICO 能编、S3 编不过"的破损状态。
 *
 *    行为上它**与 PICO 完全一致**：不上报任何事件，
 *    「聆听结束」仍由 PC 侧 RMS 决定。`afe_available()` 返回 false 就是这个意思 ——
 *    **能力已具备（S3 有 AFE），但尚未接入**。
 *
 *    接入 ESP-SR 之后，本文件会被真正的实现替换，届时 `afe_available()` 改返回 true。
 *    在那之前，S3 上的 `vad` 命令会明确报告"未接入"，而不是假装能用。
 *
 * 接入时的要点（调研中，尚未确认的项已标出）：
 *   - 组件：ESP-SR 是**托管组件**，需要 idf_component.yml 声明，
 *           并用 `rules: if: target in [esp32s3]` 限制只给 S3 拉取
 *   - ⚠️ **单麦克风怎么喂**：文档说 AFE 输入是双声道（麦克风 + AEC 参考），
 *          而我们没有参考信号 —— 这是最大的未知，未确认
 *   - ⚠️ **帧长**：我们每次 256 样本（16ms），AFE 通常要 512（32ms）→ 本层需攒帧
 *   - 只启用 VAD，**不开 AEC/NS/BSS**（单麦克风用不上，且 D13.4 要求音频路径不变）
 *   - 内存优先放 PSRAM（S3 有 8MB）
 */
#include "afe.h"

#include "board.h"

#if BOARD_HAS_ESPSR

#include "esp_log.h"

static const char *TAG = "afe_espsr";

const char *afe_name(void) { return "esp-sr(未接入)"; }

bool afe_available(void) { return false; }

esp_err_t afe_init(void)
{
    ESP_LOGW(TAG, "板端 VAD 尚未接入（等待 ESP-SR 集成）");
    ESP_LOGW(TAG, "→ 判停暂时仍走 PC 侧 RMS，与 PICO 行为一致");
    return ESP_OK;
}

void afe_feed(const int16_t *pcm, int n)
{
    (void)pcm;
    (void)n;
}

afe_event_t afe_poll_event(void) { return AFE_EVENT_NONE; }

void afe_reset(void) { }

const char *afe_vad_state_str(void) { return "not-wired"; }

#endif /* BOARD_HAS_ESPSR */
