/*
 * 串口传输：帧的收发。
 *
 * 两条设计决策都是踩过坑才定下来的：
 *
 * 1) **输入不走 stdin。** 之前用 fgets(stdin) 遇到 console 非阻塞的问题
 *    （敲进去的字符被切碎、逐个当指令）。现在直接 uart_read_bytes 自己做
 *    解析，二进制帧与文本命令靠「是否以 0xAA 0x55 开头」区分 —— 这个区分
 *    是无歧义的：帧永远以 magic 开头，而文本命令是 ASCII。
 *
 * 2) **发送阻塞无法避免，但必须如实上报。** uart_write_bytes 在 TX 环形缓冲
 *    满时会阻塞，阻塞超过 4 个 DMA 缓冲（64ms）就意味着 I²S 已经丢了音频。
 *    本模块不试图避免（做不到），而是记录累计阻塞时长；调用方再用
 *    「应发样本数 vs 实发样本数」量化丢失量。
 *    **静默丢数据比慢更糟。**
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

esp_err_t transport_init(void);

/* 发送一个帧。payload 可为 NULL（len=0） */
void transport_send(uint8_t type, const uint8_t *payload, uint16_t len);

/* 便利函数：把 C 字符串当 payload */
void transport_send_text(uint8_t type, const char *text);

/* 非阻塞轮询。
 *
 * 返回收到的 **payload 长度（>= 0）**，无帧返回 **-1**。
 * 文本行会被包装成 type = FRAME_TYPE_CONTROL 的帧返回（payload 以 '\0' 结尾，
 * 但返回值不含 '\0'）。
 */
int transport_poll(uint8_t *out_type, uint8_t *out_payload, int max_len, int timeout_ms);

void transport_get_stats(uint32_t *tx_frames, uint32_t *rx_frames, uint32_t *bad_crc);

/* 裸字节写出（不经帧封装）。给 diag 的 `wav` 命令发裸 PCM 用 ——
 * 它必须走与实际传输相同的通路，否则在 S3 上会写到 UART0（没接东西）。 */
void transport_write_raw(const uint8_t *data, size_t len);
void transport_wait_tx_done(void);

/* 累计阻塞在 uart_write_bytes 上的毫秒数。
 * 一次 DMA 缓冲 = 16ms，4 个 = 64ms —— 累计超过约 64ms 就意味着 I²S 已丢数据。 */
uint32_t transport_tx_blocked_ms(void);

/* 清零所有计数。每次开始录音前调用 —— 阻塞时长必须是**本段**的，
 * 否则累积值无法用来判断"这一段有没有因为背压丢音频" */
void transport_reset_counters(void);

/* 供诊断/状态上报：一次 DMA 缓冲对应的毫秒数（= 帧数 × 1000 / 采样率） */
#define TRANSPORT_DMA_BUFFER_MS  16
