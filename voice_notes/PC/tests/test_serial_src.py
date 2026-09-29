"""SerialSource 的纯逻辑单测 —— 不接硬件，用 __new__ 绕过 open()。

只测能脱离串口验证的部分：丢帧统计。收发本身要靠 Task 7 的端到端验收。
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from frames import TYPE_AUDIO, FrameParser, pack_frame  # noqa: E402
from sources.serial_src import SerialSource  # noqa: E402


def _bare_source() -> SerialSource:
    """造一个没打开串口的实例。

    这里列出的字段必须与 SerialSource.__init__ 保持一致 —— 少一个就会
    得到 AttributeError 而不是有意义的断言失败。"""
    src = SerialSource.__new__(SerialSource)
    src._parser = FrameParser()
    src._pending = []
    src._last_seq = None
    src.frames = 0
    src.lost = 0
    src.bad_crc = 0
    src.last_status = ""
    src.max_in_waiting = 0
    return src


class FakeSerial:
    """假串口：按预设的块依次吐出数据，用完后返回空"""

    def __init__(self, chunks):
        self._chunks = list(chunks)

    @property
    def in_waiting(self):
        """模拟 OS 缓冲区里当前可读的字节数 —— 下一个待吐出的块的长度"""
        return len(self._chunks[0]) if self._chunks else 0

    def read(self, _n):
        return self._chunks.pop(0) if self._chunks else b""

    def write(self, _b):
        pass

    def setDTR(self, _v):
        pass

    def setRTS(self, _v):
        pass

    def close(self):
        pass

    def reset_input_buffer(self):
        pass


def _source_with(chunks):
    src = _bare_source()
    src._ser = FakeSerial(chunks)
    return src


def _audio_frame(seq: int, nsamples: int = 4) -> bytes:
    payload = struct.pack("<II", seq, 0) + struct.pack(f"<{nsamples}h", *([1] * nsamples))
    return pack_frame(TYPE_AUDIO, payload)


def test_returns_every_frame_in_one_serial_chunk():
    """一次串口 read 里可能含多帧 —— 必须全部返回，不能只给第一帧。

    这是实测踩过的真 bug：read_audio 解析出一个 chunk 里的所有帧，
    却只 return 第一帧，其余全部被丢弃。

    症状（60 秒验收）：固件报 samples=961280（deficit=9，tx_block=0，
    说明固件侧完全正常），PC 只收到 293120 —— 丢了 70%，且 bad_crc=0
    （帧本身都是好的，纯粹是被扔了）。
    """
    src = _source_with([b"".join(_audio_frame(i) for i in range(5))])
    got = []
    for _ in range(5):
        fr = src.read_audio(timeout=0.5)
        if fr:
            got.append(fr.seq)
    assert got == [0, 1, 2, 3, 4]


def test_returns_frames_across_multiple_chunks():
    """跨多个 chunk 也要一帧不落"""
    src = _source_with([_audio_frame(0), _audio_frame(1), _audio_frame(2)])
    got = []
    for _ in range(3):
        fr = src.read_audio(timeout=0.5)
        if fr:
            got.append(fr.seq)
    assert got == [0, 1, 2]


def test_drain_flushes_pending_without_false_loss():
    """drain 前必须先交付 _pending，否则会产生**假的丢帧计数**。

    实测踩过：60 秒验收报 lost=2，看起来像链路丢了 2 帧，但 bad_crc=0
    （没有任何帧损坏）—— 真丢字节的话必然拼出半帧、CRC 校验失败。
    真相是：主循环结束时 _pending 里还剩几帧没交付，而 drain 期间收到的
    帧序号已经越过它们，_track 一看序号跳了就报丢帧。
    那几帧并没有丢，只是没写进 WAV（属于"停止时的边界效应"，不是传输故障）。
    """
    src = _source_with([b"".join(_audio_frame(i) for i in range(5)), _audio_frame(5)])
    src.read_audio(timeout=0.5)  # 交付 seq=0，_pending 里留 [1,2,3,4]
    assert len(src._pending) == 4

    src.drain(0.1)  # 读到 seq=5

    assert src.lost == 0, "1~5 全部收到，不应报丢帧"
    assert src.frames == 6


def test_no_loss_when_all_frames_delivered():
    """全部送达时 lost 必须是 0 —— 上一条 bug 会让 lost 虚高"""
    src = _source_with([b"".join(_audio_frame(i) for i in range(10))])
    for _ in range(10):
        src.read_audio(timeout=0.5)
    assert src.frames == 10
    assert src.lost == 0


def test_no_gap():
    src = _bare_source()
    for seq in (0, 1, 2, 3):
        src._track(seq)
    assert src.frames == 4
    assert src.lost == 0


def test_counts_gap():
    src = _bare_source()
    src._track(1)
    src._track(2)
    src._track(5)  # 缺 3、4
    assert src.frames == 3
    assert src.lost == 2


def test_accumulates_multiple_gaps():
    src = _bare_source()
    src._track(0)
    src._track(3)  # 缺 1、2
    src._track(4)
    src._track(8)  # 缺 5、6、7
    assert src.frames == 4
    assert src.lost == 5


def test_first_frame_only_sets_baseline():
    """第一帧没有前序，不能算成丢帧"""
    src = _bare_source()
    src._track(1000)
    assert src.lost == 0
    assert src.frames == 1


def test_out_of_order_does_not_underflow():
    """乱序（gap 为负）不应让 lost 变成负数或暴涨"""
    src = _bare_source()
    src._track(10)
    src._track(9)  # 倒退
    assert src.lost == 0


def test_stats_shape():
    src = _bare_source()
    src._track(0)
    st = src.stats()
    assert set(st) == {"frames", "lost", "bad_crc", "last_status", "max_in_waiting"}
    assert st["frames"] == 1
