#include "audio_frame.h"

#include <string.h>

uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;              /* init */
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            /* poly=0x1021，不反射 */
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t frame_pack(uint8_t *buf, size_t buf_size, uint8_t type,
                  const uint8_t *payload, uint16_t payload_len)
{
    const size_t total = (size_t)FRAME_OVERHEAD + payload_len;
    if (buf_size < total) return 0;     /* 不越界写，直接拒绝 */

    buf[0] = FRAME_MAGIC_0;
    buf[1] = FRAME_MAGIC_1;
    buf[2] = type;
    buf[3] = (uint8_t)(payload_len & 0xFF);         /* 小端 */
    buf[4] = (uint8_t)(payload_len >> 8);
    if (payload_len && payload) {
        memcpy(&buf[5], payload, payload_len);
    }

    /* CRC 覆盖 type + len + payload，即 buf[2 .. 5+payload_len-1] */
    const uint16_t crc = crc16_ccitt(&buf[2], 3 + payload_len);
    buf[5 + payload_len]     = (uint8_t)(crc & 0xFF);
    buf[5 + payload_len + 1] = (uint8_t)(crc >> 8);
    return total;
}

size_t audio_payload_pack(uint8_t *buf, uint32_t seq, uint32_t ts_ms,
                          const int16_t *pcm, size_t n_samples)
{
    buf[0] = (uint8_t)(seq & 0xFF);
    buf[1] = (uint8_t)((seq >> 8) & 0xFF);
    buf[2] = (uint8_t)((seq >> 16) & 0xFF);
    buf[3] = (uint8_t)((seq >> 24) & 0xFF);
    buf[4] = (uint8_t)(ts_ms & 0xFF);
    buf[5] = (uint8_t)((ts_ms >> 8) & 0xFF);
    buf[6] = (uint8_t)((ts_ms >> 16) & 0xFF);
    buf[7] = (uint8_t)((ts_ms >> 24) & 0xFF);
    if (n_samples) {
        memcpy(&buf[8], pcm, n_samples * sizeof(int16_t));
    }
    return 8 + n_samples * sizeof(int16_t);
}
