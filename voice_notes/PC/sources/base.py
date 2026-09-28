"""音频来源抽象。

存在的理由：传输要支持串口和 WiFi 两条路（见 docs/prd.md D4），
两者对上层必须**完全等价** —— 换传输不改下游一行代码。
"""
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Optional


@dataclass
class AudioFrame:
    seq: int
    ts_ms: int
    pcm: bytes  # 16-bit LE 单声道


class AudioSource(ABC):
    """音频来源。

    生命周期：open() → send_control('START') → 反复 read_audio()
              → send_control('STOP') → close()
    """

    @abstractmethod
    def open(self) -> None:
        """打开链路并等待设备就绪"""

    @abstractmethod
    def send_control(self, cmd: str) -> None:
        """发送控制命令，如 'START' / 'STOP'"""

    @abstractmethod
    def read_audio(self, timeout: float = 1.0) -> Optional[AudioFrame]:
        """读一帧音频，超时返回 None。

        非音频帧（状态等）会被内部消化，通过 last_status 暴露。
        """

    @abstractmethod
    def close(self) -> None:
        ...

    @abstractmethod
    def stats(self) -> dict:
        """返回 {frames, lost, bad_crc, last_status} 供验收判定用"""

    # 子类应设置：
    last_status: str = ""
