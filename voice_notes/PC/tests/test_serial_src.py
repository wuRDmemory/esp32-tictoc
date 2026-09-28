"""SerialSource 的纯逻辑单测 —— 不接硬件，用 __new__ 绕过 open()。

只测能脱离串口验证的部分：丢帧统计。收发本身要靠 Task 7 的端到端验收。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from frames import FrameParser  # noqa: E402
from sources.serial_src import SerialSource  # noqa: E402


def _bare_source() -> SerialSource:
    """造一个没打开串口的实例，只测统计逻辑"""
    src = SerialSource.__new__(SerialSource)
    src._parser = FrameParser()
    src._last_seq = None
    src.frames = 0
    src.lost = 0
    src.last_status = ""
    return src


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
    assert set(st) == {"frames", "lost", "bad_crc", "last_status"}
    assert st["frames"] == 1
