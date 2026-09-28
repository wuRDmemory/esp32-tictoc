"""帧协议单测（PC 侧）。

CRC 的标准测试向量 "123456789" → 0x29B1 是两端一致性的唯一锚点：
固件侧 test/host/test_audio_frame.c 也用同一向量。两边都过，才能保证互通；
否则会出现「PC 收到帧但 CRC 校验失败」这种极难定位的问题。
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from frames import (  # noqa: E402
    TYPE_AUDIO,
    TYPE_CONTROL,
    TYPE_STATUS,
    FrameParser,
    crc16_ccitt,
    pack_frame,
    unpack_audio,
)


def test_crc_known_vector():
    assert crc16_ccitt(b"123456789") == 0x29B1
    assert crc16_ccitt(b"") == 0xFFFF


def test_pack_layout():
    frame = pack_frame(TYPE_CONTROL, b"STOP")
    assert frame[0] == 0xAA and frame[1] == 0x55
    assert frame[2] == TYPE_CONTROL
    assert frame[3] == 4 and frame[4] == 0  # len u16LE
    assert frame[5:9] == b"STOP"
    want = crc16_ccitt(frame[2:9])  # 覆盖 type+len+payload
    assert frame[9] == (want & 0xFF) and frame[10] == (want >> 8)
    assert len(frame) == 11


def test_audio_payload_roundtrip():
    pcm = struct.pack("<2h", -2, 1000)
    payload = struct.pack("<II", 0x11223344, 0x55667788) + pcm
    seq, ts, got = unpack_audio(payload)
    assert (seq, ts) == (0x11223344, 0x55667788)
    assert got == pcm


def test_parser_single_frame():
    p = FrameParser()
    assert p.feed(pack_frame(TYPE_CONTROL, b"START")) == [(TYPE_CONTROL, b"START")]


def test_parser_byte_at_a_time():
    """逐字节喂 —— 串口上就是这样到达的，必须能拼回来"""
    p = FrameParser()
    frame = pack_frame(TYPE_AUDIO, b"\x01\x02\x03")
    got = []
    for b in frame:
        got += p.feed(bytes([b]))
    assert got == [(TYPE_AUDIO, b"\x01\x02\x03")]


def test_parser_skips_garbage_and_resyncs():
    """帧前混入启动日志必须能重新同步"""
    p = FrameParser()
    data = b"ets Jun  8 2016\r\nrst:0x1 (POWERON)\r\n" + pack_frame(TYPE_CONTROL, b"STOP")
    assert p.feed(data) == [(TYPE_CONTROL, b"STOP")]


def test_parser_rejects_bad_crc():
    p = FrameParser()
    frame = bytearray(pack_frame(TYPE_CONTROL, b"STOP"))
    frame[-1] ^= 0xFF  # 破坏 CRC
    assert p.feed(bytes(frame)) == []
    assert p.bad_crc_count == 1


def test_parser_handles_magic_in_payload():
    """payload 里出现 0xAA 0x55 不能导致解析错乱 —— 长度字段是权威的"""
    p = FrameParser()
    frame = pack_frame(TYPE_AUDIO, b"\xaa\x55\xaa\x55")
    assert p.feed(frame) == [(TYPE_AUDIO, b"\xaa\x55\xaa\x55")]


def test_parser_two_frames_in_one_chunk():
    p = FrameParser()
    data = pack_frame(TYPE_CONTROL, b"START") + pack_frame(TYPE_STATUS, b"OK")
    assert p.feed(data) == [(TYPE_CONTROL, b"START"), (TYPE_STATUS, b"OK")]


def test_cross_checks_with_c_implementation():
    """与固件侧 audio_frame.c 的输出逐字节比对。

    这条失败 = 两端协议漂移。先跑固件侧主机测试
    （voice_notes/test/host/test_audio_frame.c），再查这里。
    """
    import os
    import subprocess
    import tempfile

    import pytest

    root = Path(__file__).resolve().parents[2]  # voice_notes/
    c_src = root / "main" / "audio_frame.c"
    if not c_src.exists():
        pytest.skip("audio_frame.c 不存在，跳过跨端检查")

    prog = r"""
#include <stdio.h>
#include "audio_frame.h"
int main(void){
    uint8_t buf[64];
    const uint8_t p[] = {0xDE,0xAD};
    size_t n = frame_pack(buf, sizeof(buf), FRAME_TYPE_CONTROL, p, 2);
    for (size_t i = 0; i < n; i++) printf("%02X", buf[i]);
    printf("\n");
    return 0;
}"""
    with tempfile.TemporaryDirectory() as td:
        cfile = os.path.join(td, "m.c")
        with open(cfile, "w") as f:
            f.write(prog)
        exe = os.path.join(td, "m")
        r = subprocess.run(
            ["gcc", "-o", exe, cfile, str(c_src), f"-I{root / 'main'}"],
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            pytest.skip(f"主机 gcc 编译失败: {r.stderr[:200]}")
        c_hex = subprocess.run([exe], capture_output=True, text=True).stdout.strip()

    assert c_hex == pack_frame(TYPE_CONTROL, b"\xde\xad").hex().upper()
