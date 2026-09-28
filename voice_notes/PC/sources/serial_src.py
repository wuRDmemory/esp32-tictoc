"""串口音频来源（阶段 2 唯一的实现）。

每条设计都对应一个已知的坑：

1. **打开串口后立刻把 DTR/RTS 拉低。** pyserial 默认会断言这两根线，
   而它们接的是 ESP32 的 GPIO0 / EN —— 会给板子一个复位脉冲，
   导致刚发出去的 START 丢失，表现为"发起了但收不到音频"。

2. **统计丢帧用 seq 缺口，但别把它当成唯一指标。** seq 是固件生成的，
   它连续只能说明"固件发出的帧都到了"，**不能说明 I²S 没丢样本** ——
   后者要看固件在 STOPPED 状态帧里报的 deficit。

3. **串口读是非阻塞的（timeout 很小），靠循环+截止时间实现"等待"。**
   因为一帧音频只有 16ms，用大 timeout 会累积延迟。
"""
import time
from typing import Optional

import serial

from frames import TYPE_AUDIO, TYPE_STATUS, FrameParser, unpack_audio
from sources.base import AudioFrame, AudioSource


class SerialSource(AudioSource):
    def __init__(self, port: str = "/dev/ttyUSB0", baud: int = 921600) -> None:
        self.port = port
        self.baud = baud
        self._ser: Optional[serial.Serial] = None
        self._parser = FrameParser()
        self._last_seq: Optional[int] = None
        # 已解析但尚未交付的帧。见 read_audio 里的说明 —— 少了这个队列
        # 会丢掉一次串口 read 里除第一帧以外的全部数据（实测丢了 70%）。
        self._pending: list = []
        self.frames = 0
        self.lost = 0
        self.bad_crc = 0
        self.last_status = ""

    # ---------------------------------------------------------------- #
    def open(self) -> None:
        self._ser = serial.Serial(self.port, self.baud, timeout=0.05)
        self._ser.setDTR(False)
        self._ser.setRTS(False)
        self._parser.reset()
        self._pending.clear()
        self._last_seq = None
        self.frames = 0
        self.lost = 0
        self.bad_crc = 0
        self.last_status = ""

    def close(self) -> None:
        if self._ser:
            try:
                self._ser.close()
            finally:
                self._ser = None

    def wait_ready(self, timeout: float = 10.0) -> bool:
        """等板子输出就绪提示。板子还没启动完就发 START 会丢。"""
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            chunk = self._ser.read(4096)
            if chunk:
                buf += chunk
                if b"help" in buf or b"voice_notes" in buf:
                    self._ser.reset_input_buffer()
                    return True
        return False

    # ---------------------------------------------------------------- #
    def send_control(self, cmd: str) -> None:
        # 走帧协议而非裸文本 —— 这样录音过程中也能送达（帧与音频帧在
        # 同一条流里，靠 magic 区分），这正是帧协议存在的理由。
        from frames import TYPE_CONTROL, pack_frame

        self._ser.write(pack_frame(TYPE_CONTROL, cmd.encode("ascii")))

    def read_audio(self, timeout: float = 1.0) -> Optional[AudioFrame]:
        """读一帧音频。

        ⚠️ **必须用队列把一次 read 里解析出的所有帧都留住。**
        实测踩过的 bug：原实现解析出一个 chunk 里的全部帧，却只
        `return` 第一帧，其余全部被丢弃。一次 4096 字节的 read 含 7~8 帧，
        于是 60 秒录音只收到 18 秒（丢 70%），而 bad_crc=0、固件侧
        deficit=9 —— 数据本身完全正常，纯粹是在这里被扔了。
        """
        deadline = time.time() + timeout
        while True:
            # 先交付已解析但还没给出的帧
            if self._pending:
                fr = self._pending.pop(0)
                self._track(fr.seq)
                return fr

            if time.time() >= deadline:
                break

            chunk = self._ser.read(4096)
            if not chunk:
                continue

            for ftype, payload in self._parser.feed(chunk):
                if ftype == TYPE_AUDIO:
                    seq, ts, pcm = unpack_audio(payload)
                    self._pending.append(AudioFrame(seq, ts, pcm))
                elif ftype == TYPE_STATUS:
                    self.last_status = payload.decode("utf-8", "replace")

        self.bad_crc = self._parser.bad_crc_count
        return None

    def drain(self, seconds: float) -> None:
        """读掉 seconds 秒内到达的数据（用于停止后排空，收集状态帧）。

        音频帧计入统计（保持丢帧计数准确），但不保留 PCM —— 此时已经
        发过 STOP，这些是尾帧，不再需要写入 WAV。
        ⚠️ **必须先把 _pending 里剩下的帧计入统计**，否则它们与 drain 期间
        收到的帧之间会出现假的 seq 缺口，把"没写完"误报成"丢帧"。
        实测踩过：60 秒验收报 lost=2 但 bad_crc=0 —— 真丢字节的话必然
        拼出半帧、CRC 校验失败。那 2 帧其实一直躺在 _pending 里。
        """
        while self._pending:
            self._track(self._pending.pop(0).seq)

        deadline = time.time() + seconds
        while time.time() < deadline:
            chunk = self._ser.read(4096)
            if not chunk:
                continue
            for ftype, payload in self._parser.feed(chunk):
                if ftype == TYPE_AUDIO:
                    seq, _, _ = unpack_audio(payload)
                    self._track(seq)
                elif ftype == TYPE_STATUS:
                    self.last_status = payload.decode("utf-8", "replace")
        self.bad_crc = self._parser.bad_crc_count

    # ---------------------------------------------------------------- #
    def _track(self, seq: int) -> None:
        """用 seq 缺口统计丢帧。第一帧只建立基准。

        注意：这只反映"固件发出的帧有没有都到"。
        I²S 侧有没有丢样本要看固件报的 deficit（见 session.c）。
        """
        if self._last_seq is not None:
            gap = seq - self._last_seq - 1
            if gap > 0:
                self.lost += gap
        self._last_seq = seq
        self.frames += 1

    def stats(self) -> dict:
        return {
            "frames": self.frames,
            "lost": self.lost,
            "bad_crc": self._parser.bad_crc_count,
            "last_status": self.last_status,
        }
