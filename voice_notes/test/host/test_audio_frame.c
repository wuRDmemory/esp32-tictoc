/*
 * 主机侧单元测试 —— audio_frame.c 不依赖 ESP-IDF，所以能在 PC 上用 gcc 直接测。
 *
 * 这是阶段 2 唯一能脱离硬件快速迭代的部分：协议错了在主机上就能发现，
 * 不用烧进去试。
 *
 * 编译运行：
 *   gcc -Wall -Wextra -o /tmp/t_frame test/host/test_audio_frame.c \
 *       main/audio_frame.c -Imain && /tmp/t_frame
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "audio_frame.h"

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT-FALSE 的标准测试向量                                   */
/*                                                                     */
/* "123456789" → 0x29B1 是两端一致性的**唯一锚点**：                    */
/* PC 侧 tests/test_frames.py 用同一个向量。两边都过才能保证互通，      */
/* 否则会出现"PC 收到帧但 CRC 校验失败"这种极难定位的问题。             */
/* ------------------------------------------------------------------ */
static void test_crc_known_vector(void)
{
    printf("test_crc_known_vector\n");
    const uint8_t *v = (const uint8_t *)"123456789";
    CHECK(crc16_ccitt(v, 9) == 0x29B1);
    CHECK(crc16_ccitt(v, 0) == 0xFFFF);      /* 空输入 = init 值 */
}

static void test_frame_pack_layout(void)
{
    printf("test_frame_pack_layout\n");
    uint8_t buf[64];
    const uint8_t payload[] = { 0xDE, 0xAD };
    size_t n = frame_pack(buf, sizeof(buf), FRAME_TYPE_CONTROL, payload, 2);

    CHECK(n == 2 + 1 + 2 + 2 + 2);            /* magic+type+len+payload+crc */
    CHECK(buf[0] == 0xAA && buf[1] == 0x55);
    CHECK(buf[2] == FRAME_TYPE_CONTROL);
    CHECK(buf[3] == 0x02 && buf[4] == 0x00);  /* len 小端 */
    CHECK(buf[5] == 0xDE && buf[6] == 0xAD);

    /* CRC 覆盖 type+len+payload，即 buf[2..6] */
    uint16_t want = crc16_ccitt(&buf[2], 5);
    CHECK(buf[7] == (want & 0xFF));
    CHECK(buf[8] == (want >> 8));
}

static void test_frame_pack_rejects_small_buf(void)
{
    printf("test_frame_pack_rejects_small_buf\n");
    uint8_t small[8];
    const uint8_t payload[] = { 1, 2, 3, 4, 5 };
    /* 需要 7+5=12 字节，只有 8 —— 必须返回 0 而不是越界写 */
    CHECK(frame_pack(small, sizeof(small), FRAME_TYPE_CONTROL, payload, 5) == 0);
}

static void test_audio_payload(void)
{
    printf("test_audio_payload\n");
    uint8_t buf[64];
    int16_t pcm[2] = { -2, 1000 };            /* 0xFFFE, 0x03E8 */
    size_t n = audio_payload_pack(buf, 0x11223344, 0x55667788, pcm, 2);

    CHECK(n == 8 + 4);
    CHECK(buf[0] == 0x44 && buf[1] == 0x33 && buf[2] == 0x22 && buf[3] == 0x11);
    CHECK(buf[4] == 0x88 && buf[5] == 0x77 && buf[6] == 0x66 && buf[7] == 0x55);
    CHECK(buf[8] == 0xFE && buf[9] == 0xFF);  /* -2 */
    CHECK(buf[10] == 0xE8 && buf[11] == 0x03);/* 1000 */
}

/* 零长度 payload 也要能正确打包（控制帧可能为空） */
static void test_empty_payload(void)
{
    printf("test_empty_payload\n");
    uint8_t buf[16];
    size_t n = frame_pack(buf, sizeof(buf), FRAME_TYPE_STATUS, NULL, 0);
    CHECK(n == FRAME_OVERHEAD);
    CHECK(buf[3] == 0 && buf[4] == 0);
    uint16_t want = crc16_ccitt(&buf[2], 3);
    CHECK(buf[5] == (want & 0xFF) && buf[6] == (want >> 8));
}

int main(void)
{
    test_crc_known_vector();
    test_frame_pack_layout();
    test_frame_pack_rejects_small_buf();
    test_audio_payload();
    test_empty_payload();

    printf(g_fail ? "\n❌ %d 项失败\n" : "\n✅ 全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
