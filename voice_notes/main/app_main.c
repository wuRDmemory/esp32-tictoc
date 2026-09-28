/*
 * voice_notes 主程序。
 *
 * 结构很简单：一个循环，按状态分派。
 *
 *   非录音：等待输入 → START 进入录音 / 其他交给 diag 命令
 *   录音中：每次循环采一块发出，并无阻塞地看一眼有没有 STOP
 *
 * 输入全部经由 transport_poll（直接读 UART 字节），**不使用 stdin** ——
 * console 的 stdin 是行缓冲且默认非阻塞，曾经导致用户完全敲不进命令。
 */
#include <stdio.h>
#include <string.h>

#include "audio_frame.h"
#include "diag.h"
#include "i2s_mic.h"
#include "session.h"
#include "transport.h"

/* 录音中不等待：采音节奏是 16ms 一块，输入轮询不能拖慢它。
 * 空闲时等 20ms，把 CPU 让出去。 */
#define POLL_WAIT_RECORDING_MS  0
#define POLL_WAIT_IDLE_MS       20

/* 命令比对，大小写不敏感且要求整串匹配。
 * 手工敲 `start` 和 PC 发 `START` 都能用；而 `STOPPED` 不会误匹配 `STOP`。 */
static bool cmd_is(const char *s, const char *want)
{
    while (*want) {
        char a = *s++;
        char b = *want++;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return false;
    }
    return *s == '\0';
}

void app_main(void)
{
    printf("\n\n=== voice_notes ===\n");

    /* transport 必须最先初始化：它装 UART 驱动并接管 VFS，
     * 之后的 printf 才走中断驱动的发送缓冲 */
    transport_init();
    i2s_mic_init();
    diag_print_cfg();
    printf("就绪。敲 help 看命令；PC 侧发 START 开始录音。\n\n");
    fflush(stdout);

    static uint8_t payload[FRAME_MAX_PAYLOAD + 1];
    uint8_t type;

    while (1) {
        if (session_is_recording()) {
            session_tick();

            int len = transport_poll(&type, payload, FRAME_MAX_PAYLOAD,
                                     POLL_WAIT_RECORDING_MS);
            if (len >= 0 && type == FRAME_TYPE_CONTROL && cmd_is((char *)payload, "STOP")) {
                session_stop();
            }
        } else {
            int len = transport_poll(&type, payload, FRAME_MAX_PAYLOAD,
                                     POLL_WAIT_IDLE_MS);
            if (len < 0 || type != FRAME_TYPE_CONTROL) continue;

            char *cmd = (char *)payload;
            if (cmd_is(cmd, "START")) {
                session_start();
            } else {
                diag_handle_command(cmd);
            }
        }
    }
}
