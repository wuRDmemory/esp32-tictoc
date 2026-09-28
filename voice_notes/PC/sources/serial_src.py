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
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self._ser.read(4096)
            if chunk:
                for ftype, payload in self._parser.feed(chunk):
                    if ftype == TYPE_AUDIO:
                        seq, ts, pcm = unpack_audio(payload)
                        self._track(seq)
                        return AudioFrame(seq, ts, pcm)
                    if ftype == TYPE_STATUS:
                        self.last_status = payload.decode("utf-8", "replace")
        self.bad_crc = self._parser.bad_crc_count
        return None

    def drain(self, seconds: float) -> None:
        """读掉 seconds 秒内到达的所有数据但不返回（用于停止后排空）"""
        deadline = time.time() + seconds
        while time.time() < deadline:
            chunk = self._ser.read(4096)
            if chunk:
                for ftype, payload in self._parser.feed(chunk):
                    if ftype == TYPE_STATUS:
                        self.last_status = payload.decode("utf-8", "replace")

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
