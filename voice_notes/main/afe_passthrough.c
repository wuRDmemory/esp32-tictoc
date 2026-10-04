/*
 * AFE 的**空实现** —— 给经典 ESP32（PICO）用。
 *
 * 经典 ESP32 没有 AFE/VAD（docs/wakeword-research.md §2 有完整的支持矩阵），
 * 所以这里全是空操作，**永远不上报事件**。
 *
 * ⚠️ 这**不是**"占位符"或"待办"，而是**这块板上正确的行为**：
 *    PICO 的「聆听结束」继续由 PC 侧的 RMS 静音检测决定（prd.md FR-5）。
 *
 *    换句话说：**引入 afe.h 之后，PICO 的行为一个字都没变。**
 *    这正是这层抽象存在的意义 —— 换板不该改变另一块板的既有行为。
 */
#include "afe.h"

#include "board.h"

#if !BOARD_HAS_ESPSR

const char *afe_name(void) { return "passthrough"; }

bool afe_available(void) { return false; }

esp_err_t afe_init(void) { return ESP_OK; }

void afe_feed(const int16_t *pcm, int n)
{
    (void)pcm;
    (void)n;
}

afe_event_t afe_poll_event(void) { return AFE_EVENT_NONE; }

void afe_reset(void) { }

const char *afe_vad_state_str(void) { return "n/a"; }

#endif /* !BOARD_HAS_ESPSR */
