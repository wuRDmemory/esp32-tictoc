/*
 * 录音会话状态机。
 *
 *   IDLE ──START──> RECORDING ──STOP──> IDLE
 *
 * 固件侧只管"采音、打包、发送"，不管语义（不做识别、不做判断）。
 * 用户按停 / 静音超时 / 敲命令，都由 PC 侧决定后发 STOP 过来。
 */
#pragma once

#include <stdbool.h>

#define SESSION_CHUNK_SAMPLES  256   /* = I2S_MIC_FRAMES_PER_READ，16ms @16kHz */

bool session_is_recording(void);

void session_start(void);
void session_stop(void);

/* 录音中每次循环调用一次：读一块 → 打包 → 发送 */
void session_tick(void);
