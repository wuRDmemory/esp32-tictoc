/*
 * 录音会话状态机。
 *
 *   IDLE ──START──> RECORDING ──STOP──> IDLE
 *                                 ▲
 *                                 └── 两个来源：外部命令 / 板端 VAD 判停
 *
 * 固件侧只管"采音、打包、发送"，**不做内容识别**（不跑 ASR、不理解说了什么）。
 *
 * ⚠️ 但自从 D13 起，固件**第一次有了"语义判断"能力** ——
 *    S3 上可用板端 AFE VAD 判断"说完了没"，自己发起 STOP（`by=VAD`）。
 *    **边界**：判的是"有没有人在说"，**不是"说了什么"**。
 *    PICO 上没有 AFE，仍由 PC 侧 RMS 判停后发 STOP 过来（`by=CMD`）。
 */
#pragma once

#include <stdbool.h>

#define SESSION_CHUNK_SAMPLES  256   /* = I2S_MIC_FRAMES_PER_READ，16ms @16kHz */

bool session_is_recording(void);

void session_start(void);
void session_stop(void);

/* 录音中每次循环调用一次：读一块 → 打包 → 发送 */
void session_tick(void);
