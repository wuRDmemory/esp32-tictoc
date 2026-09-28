/*
 * 帧协议：串口与 TCP 共用，字节级定义见 docs/prd.md §7
 *
 * 本文件是**纯 C，不依赖 ESP-IDF** —— 因此可以在 PC 上用 gcc 直接单测
 * （见 test/host/test_audio_frame.c）。改动后务必跑一次主机测试，
 * 并同时跑 PC 侧的 voice_notes/PC/tests/test_frames.py 确认两端仍互通。
 *
 * 帧布局：
 *   偏移  长度  字段
 *   0     2     magic 0xAA 0x55
 *   2     1     type
 *   3     2     len    u16 小端
 *   5     N     payload
 *   5+N   2     crc16  CRC-16/CCITT-FALSE，覆盖 type+len+payload
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define FRAME_MAGIC_0       0xAA
#define FRAME_MAGIC_1       0x55

#define FRAME_TYPE_AUDIO    0x01    /* ESP → PC */
#define FRAME_TYPE_CONTROL  0x02    /* PC → ESP */
#define FRAME_TYPE_STATUS   0x03    /* ESP → PC */

/* magic(2) + type(1) + len(2) + crc(2) */
#define FRAME_OVERHEAD      7
#define FRAME_MAX_PAYLOAD   1024

/* CRC-16/CCITT-FALSE: poly=0x1021 init=0xFFFF 不反射 xorout=0
 * 标准测试向量 "123456789" → 0x29B1 */
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

/* 打包一个完整帧。buf 不足返回 0，否则返回帧总长 */
size_t frame_pack(uint8_t *buf, size_t buf_size, uint8_t type,
                  const uint8_t *payload, uint16_t payload_len);

/* 音频 payload: seq u32LE | ts_ms u32LE | pcm[]。返回 payload 长度 */
size_t audio_payload_pack(uint8_t *buf, uint32_t seq, uint32_t ts_ms,
                          const int16_t *pcm, size_t n_samples);
