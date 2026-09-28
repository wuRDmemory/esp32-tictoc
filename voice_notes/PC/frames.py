"""帧协议（PC 侧）。

⚠️ 本文件的字节布局必须与固件侧 main/audio_frame.c **严格一致**。
   改动任何一边，都要跑两边的测试确认仍然互通：
     固件侧： gcc -Wall -Wextra -o /tmp/t voice_notes/test/host/test_audio_frame.c \
                      voice_notes/main/audio_frame.c -Ivoice_notes/main && /tmp/t
     PC 侧：  python -m pytest voice_notes/PC/tests/test_frames.py
   锚点：CRC-16/CCITT-FALSE("123456789") == 0x29B1

帧布局（见 docs/prd.md §7）：
    偏移  长度  字段
    0     2     magic 0xAA 0x55
    2     1     type
    3     2     len    u16 小端
    5     N     payload
    5+N   2     crc16  覆盖 type+len+payload
"""
import struct
from typing import List, Tuple

MAGIC = b"\xAA\x55"

TYPE_AUDIO = 0x01    # ESP → PC
TYPE_CONTROL = 0x02  # PC → ESP
TYPE_STATUS = 0x03   # ESP → PC

HEADER_LEN = 5     # magic(2) + type(1) + len(2)
OVERHEAD = 7       # header + crc(2)
MAX_PAYLOAD = 1024


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021 init=0xFFFF 不反射 xorout=0"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def pack_frame(ftype: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload 过长: {len(payload)} > {MAX_PAYLOAD}")
    head = bytes([ftype]) + struct.pack("<H", len(payload))  # type(1) + len(2)
    crc = crc16_ccitt(head + payload)                        # 覆盖 type+len+payload
    return MAGIC + head + payload + struct.pack("<H", crc)


def unpack_audio(payload: bytes) -> Tuple[int, int, bytes]:
    """音频 payload → (seq, ts_ms, pcm)"""
    if len(payload) < 8:
        raise ValueError(f"音频 payload 过短: {len(payload)}")
    seq, ts_ms = struct.unpack("<II", payload[:8])
    return seq, ts_ms, payload[8:]


class FrameParser:
    """增量解析器。串口数据是流式到达的，必须能处理「半帧」。

    设计要点：

    - **只在期望帧头的位置找 magic。** payload 里出现 0xAA 0x55 不会误判，
      因为长度字段是权威的 —— 收到头之后按 len 精确取体，不再扫描。
    - **CRC 失败时丢掉 1 字节继续**，而不是丢掉整帧。因为 CRC 错往往意味着
      当前这个"magic"只是碰巧撞上的假同步，真帧头可能就在后面一两个字节处。
    - **不抛异常。** 串口偶发噪声是正常现象，调用方通过 bad_crc_count 感知。
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._bad_crc = 0

    @property
    def bad_crc_count(self) -> int:
        return self._bad_crc

    def reset(self) -> None:
        self._buf.clear()

    def feed(self, data: bytes) -> List[Tuple[int, bytes]]:
        self._buf += data
        out: List[Tuple[int, bytes]] = []

        while True:
            i = self._buf.find(MAGIC)
            if i < 0:
                # 没有 magic。只保留最后 1 字节，防止 magic 被切成两半
                # （另一半还在路上，下一轮 feed 才到）
                del self._buf[: max(0, len(self._buf) - 1)]
                break
            if i > 0:
                del self._buf[:i]           # 丢掉 magic 之前的垃圾（如启动日志）

            if len(self._buf) < HEADER_LEN:
                break                        # 头还没收全

            plen = self._buf[3] | (self._buf[4] << 8)
            if plen > MAX_PAYLOAD:
                # 长度不合理 —— 大概率是假同步，丢 1 字节重找
                self._bad_crc += 1
                del self._buf[:1]
                continue

            total = OVERHEAD + plen
            if len(self._buf) < total:
                break                        # 体还没收全

            body = bytes(self._buf[2 : HEADER_LEN + plen])
            want = self._buf[HEADER_LEN + plen] | (self._buf[HEADER_LEN + plen + 1] << 8)
            if crc16_ccitt(body) == want:
                out.append((self._buf[2], bytes(self._buf[HEADER_LEN : HEADER_LEN + plen])))
                del self._buf[:total]
            else:
                self._bad_crc += 1
                del self._buf[:1]
        return out
