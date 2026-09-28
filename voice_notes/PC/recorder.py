"""边收边写 WAV。

为什么不攒在内存里再写：
- 录音时长只受磁盘限制，不受内存限制
- 停止时文件已经写好，可以直接交给 ASR（阶段 3 的 FunASR 需要文件路径）
- 万一程序崩溃，已录到的部分还在
"""
import wave
from pathlib import Path


class WavRecorder:
    def __init__(self, path, rate: int = 16000, channels: int = 1) -> None:
        self.path = Path(path)
        self.rate = rate
        self._w = wave.open(str(self.path), "wb")
        self._w.setnchannels(channels)
        self._w.setsampwidth(2)  # 16-bit
        self._w.setframerate(rate)
        self._closed = False

    def feed(self, pcm: bytes) -> None:
        if not self._closed:
            self._w.writeframes(pcm)

    @property
    def samples(self) -> int:
        return self._w.getnframes()

    def close(self) -> int:
        """关闭并返回总样本数"""
        if not self._closed:
            self._w.close()
            self._closed = True
        return self.samples

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False
