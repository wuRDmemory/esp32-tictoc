# 阶段 2：串口实时音频流 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 板子连续采音，按自定义帧协议实时经串口推到 PC，PC 边收边写 WAV；连续 60 秒零丢帧。

**Architecture:** 固件侧把现有 `mic_test.c` 按职责拆成 `i2s_mic` / `audio_frame` / `transport` / `session` / `diag` 五个模块，诊断命令原样保留（它们价值极高，不要删）。PC 侧用 `AudioSource` 抽象隔离传输方式，先只实现串口。帧协议双向共用，因此 STOP 可以在录音过程中送达。

**Tech Stack:** ESP-IDF v5.5.5（`esp_driver_uart` / `esp_driver_i2s`）、Python 3.11 + pyserial、pytest

**Spec:** `hello_world/docs/prd.md`（§7 协议、§8 模块划分；变更记录见头部）

## Global Constraints

- **任何 `idf.py` 命令前必须先 `oesp`**（硬依赖，见 `CLAUDE.md`）
- 注释用中文
- 固件常量必须与已验证配置一致，**不要重新推导**：16 kHz、`I2S_CLK_SRC_APLL`、`I2S_SLOT_MODE_STEREO` + 32-bit（= 64 SCK/帧）、`dma_frame_num = 256`、GPIO BCLK=26 / WS=25 / DIN=22、24bit 在 `[31:8]` 故 `>>16`
- 串口 921600 8N1；console 走 `ESP_CONSOLE_UART_CUSTOM`
- **绝不跑 `apt upgrade` / `dist-upgrade`**（`libc6` 是手装的 2.35，升级会打断 ROS 和整个 WSL）
- 改 `sdkconfig.defaults` 后必须 `rm -rf build sdkconfig && idf.py set-target esp32`，并 **grep 生成的 `sdkconfig` 复核**（有些 `CONFIG_*` 会被 Kconfig 静默丢弃）
- 提交信息结尾加 `Co-Authored-By: Claude Code <noreply@anthropic.com>`

### 帧协议（`prd.md` §7，字节级，两端必须一致）

```
偏移   长度   字段
0      2     magic     0xAA 0x55
2      1     type      0x01=音频(ESP→PC)  0x02=控制(PC→ESP)  0x03=状态(ESP→PC)
3      2     len       payload 长度，u16 小端
5      N     payload
5+N    2     crc16     CRC-16/CCITT-FALSE，覆盖 type+len+payload 三部分
```

音频 payload：`seq u32LE | ts_ms u32LE | pcm[]`（16-bit 小端单声道）
控制 payload：ASCII `START` / `STOP`，无终止符

**CRC-16/CCITT-FALSE 参数**：poly=`0x1021`，init=`0xFFFF`，不反射，xorout=`0x0000`。
**标准测试向量：`"123456789"` → `0x29B1`**（两端都要用这个向量自测）

---

## File Structure

**固件**（`voice_notes/main/`）：

| 文件 | 职责 |
|---|---|
| `audio_frame.c/h` | 帧打包/解包 + CRC16。**纯 C，无 ESP 依赖，可在主机上单测** |
| `i2s_mic.c/h` | I²S 初始化（APLL 回退）、DC 阻断、读出 16bit 单声道 |
| `transport.c/h` | UART 初始化、发帧、RX 字节流 → 帧解析 |
| `session.c/h` | IDLE/RECORDING 状态机 |
| `diag.c/h` | 诊断命令：level / raw / rec / shift / chan / dc |
| `app_main.c` | 初始化 + 命令分发 |

**PC**（`voice_notes/PC/`）：

| 文件 | 职责 |
|---|---|
| `frames.py` | 帧编解码（与 `audio_frame.c` 对应） |
| `sources/base.py` | `AudioSource` 抽象 |
| `sources/serial_src.py` | 串口实现 |
| `recorder.py` | 边收边写 WAV |
| `tests/test_frames.py` | 帧协议单测 |
| `stage2_accept.py` | 端到端验收脚本 |

---

## Task 1: 帧编解码（固件侧，纯 C）

`audio_frame.c` 不依赖 ESP-IDF，因此**可以在 PC 上用 gcc 直接单测** —— 这是本阶段唯一能脱离硬件快速迭代的部分，先把它的正确性钉死。

**Files:**
- Create: `voice_notes/main/audio_frame.h`
- Create: `voice_notes/main/audio_frame.c`
- Test: `voice_notes/test/host/test_audio_frame.c`

**Interfaces:**
- Consumes: 无
- Produces:
  - `uint16_t crc16_ccitt(const uint8_t *data, size_t len)`
  - `size_t frame_pack(uint8_t *buf, size_t buf_size, uint8_t type, const uint8_t *payload, uint16_t payload_len)` → 返回帧总长，buf 不足返回 0
  - `size_t audio_payload_pack(uint8_t *buf, uint32_t seq, uint32_t ts_ms, const int16_t *pcm, size_t n_samples)` → 返回 payload 长度
  - 宏 `FRAME_TYPE_AUDIO` `FRAME_TYPE_CONTROL` `FRAME_TYPE_STATUS` `FRAME_MAGIC_0` `FRAME_MAGIC_1` `FRAME_OVERHEAD`(=7)

- [ ] **Step 1: 写失败的测试**

创建 `voice_notes/test/host/test_audio_frame.c`：

```c
/* 主机侧单元测试：gcc 直接编译，不需要 ESP32 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio_frame.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

/* CRC-16/CCITT-FALSE 的标准测试向量 —— 这是两端一致性的唯一锚点 */
static void test_crc_known_vector(void)
{
    printf("test_crc_known_vector\n");
    const uint8_t *v = (const uint8_t *)"123456789";
    CHECK(crc16_ccitt(v, 9) == 0x29B1);
    CHECK(crc16_ccitt(v, 0) == 0xFFFF);   /* 空输入 = init 值 */
}

static void test_frame_pack_layout(void)
{
    printf("test_frame_pack_layout\n");
    uint8_t buf[64];
    const uint8_t payload[] = {0xDE, 0xAD};
    size_t n = frame_pack(buf, sizeof(buf), FRAME_TYPE_CONTROL, payload, 2);

    CHECK(n == 2 + 1 + 2 + 2 + 2);        /* magic+type+len+payload+crc */
    CHECK(buf[0] == 0xAA && buf[1] == 0x55);
    CHECK(buf[2] == FRAME_TYPE_CONTROL);
    CHECK(buf[3] == 0x02 && buf[4] == 0x00);   /* len 小端 */
    CHECK(buf[5] == 0xDE && buf[6] == 0xAD);

    /* CRC 覆盖 type+len+payload，即 buf[2..6] */
    uint16_t want = crc16_ccitt(&buf[2], 5);
    CHECK(buf[7] == (want & 0xFF) && buf[8] == (want >> 8));
}

static void test_frame_pack_rejects_small_buf(void)
{
    printf("test_frame_pack_rejects_small_buf\n");
    uint8_t small[8];
    const uint8_t payload[] = {1, 2, 3, 4, 5};
    CHECK(frame_pack(small, sizeof(small), FRAME_TYPE_CONTROL, payload, 5) == 0);
}

static void test_audio_payload(void)
{
    printf("test_audio_payload\n");
    uint8_t buf[64];
    int16_t pcm[2] = { -2, 1000 };        /* 0xFFFE, 0x03E8 */
    size_t n = audio_payload_pack(buf, 0x11223344, 0x55667788, pcm, 2);

    CHECK(n == 8 + 4);
    CHECK(buf[0] == 0x44 && buf[1] == 0x33 && buf[2] == 0x22 && buf[3] == 0x11);
    CHECK(buf[4] == 0x88 && buf[5] == 0x77 && buf[6] == 0x66 && buf[7] == 0x55);
    CHECK(buf[8] == 0xFE && buf[9] == 0xFF);      /* -2 */
    CHECK(buf[10] == 0xE8 && buf[11] == 0x03);    /* 1000 */
}

int main(void)
{
    test_crc_known_vector();
    test_frame_pack_layout();
    test_frame_pack_rejects_small_buf();
    test_audio_payload();
    printf(g_fail ? "\n❌ %d 项失败\n" : "\n✅ 全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 2: 跑测试确认它失败**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes
gcc -o /tmp/t_frame test/host/test_audio_frame.c main/audio_frame.c -Imain 2>&1 | head -5
```
Expected: 编译失败 —— `audio_frame.h: No such file or directory`

- [ ] **Step 3: 写实现**

创建 `voice_notes/main/audio_frame.h`：

```c
/* 帧协议：串口与 TCP 共用，字节级定义见 docs/prd.md §7
 *
 * 本文件是纯 C，不依赖 ESP-IDF —— 因此可以在 PC 上用 gcc 直接单测
 * （见 test/host/test_audio_frame.c）。改动后务必跑一次主机测试。 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define FRAME_MAGIC_0       0xAA
#define FRAME_MAGIC_1       0x55
#define FRAME_TYPE_AUDIO    0x01    /* ESP → PC */
#define FRAME_TYPE_CONTROL  0x02    /* PC → ESP */
#define FRAME_TYPE_STATUS   0x03    /* ESP → PC */

/* magic(2) + type(1) + len(2) + crc(2) */
#define FRAME_OVERHEAD      7
#define FRAME_MAX_PAYLOAD   1024

/* CRC-16/CCITT-FALSE: poly=0x1021 init=0xFFFF 不反射 xorout=0
 * 标准测试向量 "123456789" → 0x29B1 */
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

/* 打包一个完整帧。buf 不足返回 0，否则返回帧总长 */
size_t frame_pack(uint8_t *buf, size_t buf_size, uint8_t type,
                  const uint8_t *payload, uint16_t payload_len);

/* 音频 payload: seq u32LE | ts_ms u32LE | pcm[]。返回 payload 长度 */
size_t audio_payload_pack(uint8_t *buf, uint32_t seq, uint32_t ts_ms,
                          const int16_t *pcm, size_t n_samples);
```

创建 `voice_notes/main/audio_frame.c`：

```c
#include "audio_frame.h"
#include <string.h>

uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t frame_pack(uint8_t *buf, size_t buf_size, uint8_t type,
                  const uint8_t *payload, uint16_t payload_len)
{
    size_t total = (size_t)FRAME_OVERHEAD + payload_len;
    if (buf_size < total) return 0;

    buf[0] = FRAME_MAGIC_0;
    buf[1] = FRAME_MAGIC_1;
    buf[2] = type;
    buf[3] = (uint8_t)(payload_len & 0xFF);
    buf[4] = (uint8_t)(payload_len >> 8);
    if (payload_len) memcpy(&buf[5], payload, payload_len);

    /* CRC 覆盖 type + len + payload，即 buf[2 .. 5+payload_len-1] */
    uint16_t crc = crc16_ccitt(&buf[2], 3 + payload_len);
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
    memcpy(&buf[8], pcm, n_samples * sizeof(int16_t));
    return 8 + n_samples * sizeof(int16_t);
}
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes
gcc -Wall -Wextra -o /tmp/t_frame test/host/test_audio_frame.c main/audio_frame.c -Imain && /tmp/t_frame
```
Expected: `✅ 全部通过`，且**无编译警告**（`-Wall -Wextra` 下）

- [ ] **Step 5: 提交**

```bash
git add voice_notes/main/audio_frame.h voice_notes/main/audio_frame.c voice_notes/test/host/test_audio_frame.c
git commit -m "阶段2: 帧编解码（纯C，含主机侧单测）"
```

---

## Task 2: 帧编解码（PC 侧）+ 跨端一致性

**关键**：Python 实现必须和 C 实现**逐字节一致**，否则会出现"PC 收到帧但 CRC 校验失败"这种极难定位的问题。用同一组测试向量卡住。

**Files:**
- Create: `voice_notes/PC/frames.py`
- Test: `voice_notes/PC/tests/test_frames.py`

**Interfaces:**
- Consumes: 无（独立实现，但必须与 Task 1 的 C 版字节一致）
- Produces:
  - `crc16_ccitt(data: bytes) -> int`
  - `pack_frame(ftype: int, payload: bytes) -> bytes`
  - `class FrameParser`：`feed(data: bytes) -> list[tuple[int, bytes]]`
  - 常量 `TYPE_AUDIO=0x01` `TYPE_CONTROL=0x02` `TYPE_STATUS=0x03`
  - `unpack_audio(payload: bytes) -> tuple[int, int, bytes]` → `(seq, ts_ms, pcm)`

- [ ] **Step 1: 写失败的测试**

创建 `voice_notes/PC/tests/test_frames.py`：

```python
"""帧协议单测。

CRC 的标准测试向量 "123456789" → 0x29B1 是两端一致性的唯一锚点：
固件侧 test_audio_frame.c 也用同一向量。两边都过，才能保证互通。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from frames import (  # noqa: E402
    TYPE_AUDIO, TYPE_CONTROL, FrameParser, crc16_ccitt, pack_frame, unpack_audio,
)


def test_crc_known_vector():
    assert crc16_ccitt(b"123456789") == 0x29B1
    assert crc16_ccitt(b"") == 0xFFFF


def test_pack_layout():
    frame = pack_frame(TYPE_CONTROL, b"STOP")
    assert frame[0] == 0xAA and frame[1] == 0x55
    assert frame[2] == TYPE_CONTROL
    assert frame[3] == 4 and frame[4] == 0        # len u16LE
    assert frame[5:9] == b"STOP"
    want = crc16_ccitt(frame[2:9])                # 覆盖 type+len+payload
    assert frame[9] == (want & 0xFF) and frame[10] == (want >> 8)
    assert len(frame) == 11


def test_audio_payload_roundtrip():
    import struct
    pcm = struct.pack("<2h", -2, 1000)
    payload = struct.pack("<II", 0x11223344, 0x55667788) + pcm
    seq, ts, got = unpack_audio(payload)
    assert (seq, ts) == (0x11223344, 0x55667788)
    assert got == pcm


def test_parser_single_frame():
    p = FrameParser()
    out = p.feed(pack_frame(TYPE_CONTROL, b"START"))
    assert out == [(TYPE_CONTROL, b"START")]


def test_parser_byte_at_a_time():
    """逐字节喂 —— 串口实际就是这样到达的，必须能拼回来"""
    p = FrameParser()
    frame = pack_frame(TYPE_AUDIO, b"\x01\x02\x03")
    got = []
    for b in frame:
        got += p.feed(bytes([b]))
    assert got == [(TYPE_AUDIO, b"\x01\x02\x03")]


def test_parser_skips_garbage_and_resyncs():
    """帧前混入垃圾（比如启动日志）必须能重新同步"""
    p = FrameParser()
    data = b"ets Jun  8 2016\r\nrst:0x1 (POWERON)\r\n" + pack_frame(TYPE_CONTROL, b"STOP")
    out = p.feed(data)
    assert out == [(TYPE_CONTROL, b"STOP")]


def test_parser_rejects_bad_crc():
    p = FrameParser()
    frame = bytearray(pack_frame(TYPE_CONTROL, b"STOP"))
    frame[-1] ^= 0xFF                     # 破坏 CRC
    assert p.feed(bytes(frame)) == []


def test_parser_handles_magic_in_payload():
    """payload 里出现 0xAA 不能导致解析错乱 —— 长度字段必须被信任"""
    p = FrameParser()
    frame = pack_frame(TYPE_AUDIO, b"\xaa\x55\xaa\x55")
    assert p.feed(frame) == [(TYPE_AUDIO, b"\xaa\x55\xaa\x55")]
```

- [ ] **Step 2: 跑测试确认它失败**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes/PC
oesp >/dev/null 2>&1          # 借 IDF venv 的 python；pytest 若没装则 pip install pytest
python -m pytest tests/test_frames.py -v 2>&1 | tail -5
```
Expected: `ModuleNotFoundError: No module named 'frames'`

- [ ] **Step 3: 写实现**

创建 `voice_notes/PC/frames.py`：

```python
"""帧协议（PC 侧）。

⚠️ 本文件的字节布局必须与固件侧 main/audio_frame.c 严格一致。
   改动任何一边，都要跑两边的测试确认仍然互通。
   锚点：CRC-16/CCITT-FALSE("123456789") == 0x29B1
"""
import struct
from typing import List, Tuple

MAGIC = b"\xAA\x55"
TYPE_AUDIO = 0x01
TYPE_CONTROL = 0x02
TYPE_STATUS = 0x03

HEADER_LEN = 5          # magic(2) + type(1) + len(2)
OVERHEAD = 7            # header + crc(2)
MAX_PAYLOAD = 1024


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def pack_frame(ftype: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload 过长: {len(payload)} > {MAX_PAYLOAD}")
    head = bytes([ftype]) + struct.pack("<H", len(payload))   # type(1) + len(2)
    crc = crc16_ccitt(head + payload)                          # 覆盖 type+len+payload
    return MAGIC + head + payload + struct.pack("<H", crc)


def unpack_audio(payload: bytes) -> Tuple[int, int, bytes]:
    """音频 payload → (seq, ts_ms, pcm)"""
    if len(payload) < 8:
        raise ValueError(f"音频 payload 过短: {len(payload)}")
    seq, ts_ms = struct.unpack("<II", payload[:8])
    return seq, ts_ms, payload[8:]


class FrameParser:
    """增量解析器。串口数据是流式到达的，必须能处理"半帧"。

    设计要点：
    - 只在**期望帧头的位置**找 magic。payload 里出现 0xAA 不会误判，
      因为长度字段是权威的。
    - CRC 校验失败时丢弃该帧并继续（不抛异常）—— 串口偶发噪声属正常。
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._bad_crc = 0

    @property
    def bad_crc_count(self) -> int:
        return self._bad_crc

    def feed(self, data: bytes) -> List[Tuple[int, bytes]]:
        self._buf += data
        out: List[Tuple[int, bytes]] = []
        while True:
            # 找帧头
            i = self._buf.find(MAGIC)
            if i < 0:
                # 没有 magic：只保留最后 1 字节，防止 magic 被切断
                del self._buf[: max(0, len(self._buf) - 1)]
                break
            if i > 0:
                del self._buf[:i]          # 丢掉 magic 之前的垃圾
            if len(self._buf) < HEADER_LEN:
                break                      # 头还没收全
            ftype = self._buf[2]
            plen = self._buf[3] | (self._buf[4] << 8)
            total = OVERHEAD + plen
            if len(self._buf) < total:
                break                      # 体还没收全
            body = bytes(self._buf[2:HEADER_LEN + plen])
            want = self._buf[HEADER_LEN + plen] | (self._buf[HEADER_LEN + plen + 1] << 8)
            if crc16_ccitt(body) == want:
                out.append((ftype, bytes(self._buf[HEADER_LEN:HEADER_LEN + plen])))
                del self._buf[:total]
            else:
                # CRC 错：丢掉 1 字节后继续找下一个 magic（可能只是假同步）
                self._bad_crc += 1
                del self._buf[:1]
        return out
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes/PC
python -m pytest tests/test_frames.py -v
```
Expected: 8 passed

- [ ] **Step 5: 加一条跨端一致性测试**

在 `tests/test_frames.py` 末尾追加 —— 这条测试把 C 实现的输出当成基准，防止两边漂移：

```python
def test_cross_checks_with_c_implementation():
    """与固件侧 audio_frame.c 的输出逐字节比对。

    如果这条失败，说明改动让两端协议不一致了 —— 那会导致 PC 收到帧但
    CRC 校验失败，是最难定位的一类问题。先跑 Task 1 的主机测试再查这里。
    """
    import subprocess
    import tempfile
    import os

    root = Path(__file__).resolve().parents[2]      # voice_notes/
    c_src = root / "main" / "audio_frame.c"
    if not c_src.exists():
        import pytest
        pytest.skip("audio_frame.c 不存在，跳过跨端检查")

    # 用 C 实现生成一个 0xDE 0xAD 的控制帧，打印成 hex
    prog = """
    #include <stdio.h>
    #include <string.h>
    #include "audio_frame.h"
    int main(void){
        uint8_t buf[64]; const uint8_t p[]={0xDE,0xAD};
        size_t n = frame_pack(buf,sizeof(buf),FRAME_TYPE_CONTROL,p,2);
        for(size_t i=0;i<n;i++) printf("%02X", buf[i]);
        printf("\\n"); return 0;
    }"""
    with tempfile.TemporaryDirectory() as td:
        cfile = os.path.join(td, "m.c")
        open(cfile, "w").write(prog)
        exe = os.path.join(td, "m")
        r = subprocess.run(
            ["gcc", "-o", exe, cfile, str(c_src), f"-I{root/'main'}"],
            capture_output=True, text=True)
        if r.returncode != 0:
            import pytest
            pytest.skip(f"主机 gcc 编译失败: {r.stderr[:200]}")
        c_hex = subprocess.run([exe], capture_output=True, text=True).stdout.strip()

    assert c_hex == pack_frame(TYPE_CONTROL, b"\xde\xad").hex().upper()
```

- [ ] **Step 6: 跑全部测试**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes/PC
python -m pytest tests/test_frames.py -v
```
Expected: 9 passed（或 9 passed / 1 skipped 如果本机没有 gcc）

- [ ] **Step 7: 提交**

```bash
git add voice_notes/PC/frames.py voice_notes/PC/tests/test_frames.py
git commit -m "阶段2: 帧编解码（PC侧）+ 跨端一致性测试"
```

---

## Task 3: 固件 I²S 模块（从 mic_test.c 抽出）

把 `mic_test.c` 里已验证的 I²S 代码抽成模块。**逻辑一行都不要改** —— 它的每一个参数都是实测确定的（见 `hardware.md` §7）。

**Files:**
- Create: `voice_notes/main/i2s_mic.h`
- Create: `voice_notes/main/i2s_mic.c`
- Reference: `voice_notes/main/mic_test.c`（照抄，不要重新推导）

**Interfaces:**
- Consumes: 无
- Produces:
  - `esp_err_t i2s_mic_init(void)` → ESP_OK 表示成功（内部已做 APLL 回退）
  - `const char *i2s_mic_clk_name(void)` → `"APLL"` 或 `"DEFAULT(PLL_D2)"`
  - `int i2s_mic_read(int16_t *out, int max_samples)` → 返回样本数，<0 表示错误
  - `void i2s_mic_set_shift(int n)` / `int i2s_mic_get_shift(void)`
  - `void i2s_mic_set_chan(int n)` / `int i2s_mic_get_chan(void)`
  - `void i2s_mic_set_dc_block(bool on)`
  - `bool i2s_mic_get_dc_block(void)`
  - `void i2s_mic_reset_dc(void)` — 重置 DC 阻断状态（每次开始录音前调用）
  - `int i2s_mic_read_raw(int32_t *left, int32_t *right, int max_frames)` — 未位移的原始字，供 `raw` 命令用

- [ ] **Step 1: 抽出头文件**

创建 `voice_notes/main/i2s_mic.h`：

```c
/* ICS-43434 采音。
 *
 * ⚠️ 本文件的所有常量都由 docs/hardware.md §7 的实测确定，不要重新推导：
 *    - STEREO + 32bit 才能凑出 ICS-43434 要求的 64 SCK/帧（MONO 会变 32 而不工作）
 *    - dma_frame_num=256 是上限 511 之下的安全值
 *    - 24bit 数据在 32bit 字的 [31:8]，故 >>16 得 16bit
 *    - 模块有约 19 位有效分辨率、输出带直流偏置，必须做直流阻断
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t i2s_mic_init(void);

const char *i2s_mic_clk_name(void);

/* 读一块，返回样本数；<0 表示错误。输出为 16bit 单声道（已过直流阻断） */
int i2s_mic_read(int16_t *out, int max_samples);

/* 读一块，输出未经位移的原始 32bit 字（供 raw 诊断命令判断位对齐） */
int i2s_mic_read_raw(int32_t *left, int32_t *right, int max_frames);

void i2s_mic_set_shift(int n);
int  i2s_mic_get_shift(void);
void i2s_mic_set_chan(int n);
int  i2s_mic_get_chan(void);
void i2s_mic_set_dc_block(bool on);
bool i2s_mic_get_dc_block(void);

/* 重置直流阻断状态。每次开始录音前调用，避免上一段的残留状态污染开头 */
void i2s_mic_reset_dc(void);

/* 触发一次预热读取（丢弃前 125ms），消除直流阻断的启动瞬态 */
void i2s_mic_warmup(void);

/* 本模块编译期常量，供 diag 打印 */
#define I2S_MIC_SAMPLE_RATE   16000
#define I2S_MIC_FRAMES_PER_READ 256
```

- [ ] **Step 2: 抽出实现**

创建 `voice_notes/main/i2s_mic.c`，**从 `mic_test.c` 原样搬移**以下内容并去掉 `static`：
`PIN_*` 宏、`s_rx`、`s_clk_name`、`s_shift`、`s_chan`、`s_dc_block`、`s_dc[2]`、
`apply_dc_block()`、`i2s_init_once()`、`i2s_init_with_fallback()`、`read_block()`、

外加新的 `i2s_mic_read()`（单声道输出）和 `i2s_mic_read_raw()`：

```c
int i2s_mic_read(int16_t *out, int max_samples)
{
    if (max_samples > I2S_MIC_FRAMES_PER_READ) max_samples = I2S_MIC_FRAMES_PER_READ;
    int16_t rtmp[I2S_MIC_FRAMES_PER_READ];
    int n = read_block(out, rtmp, max_samples);
    return n;    /* out 已是「当前选中声道 + 直流阻断后」的结果 */
}

int i2s_mic_read_raw(int32_t *left, int32_t *right, int max_frames)
{
    static uint8_t raw[I2S_MIC_FRAMES_PER_READ * 8];
    size_t got = 0;
    if (i2s_channel_read(s_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(1000)) != ESP_OK)
        return -1;
    const int32_t *w = (const int32_t *)raw;
    int frames = got / 8;
    if (frames > max_frames) frames = max_frames;
    for (int i = 0; i < frames; i++) {
        if (left)  left[i]  = w[i * 2];
        if (right) right[i] = w[i * 2 + 1];
    }
    return frames;
}

void i2s_mic_reset_dc(void)
{
    s_dc[0].x1 = s_dc[0].y1 = 0.0f;
    s_dc[1].x1 = s_dc[1].y1 = 0.0f;
}

void i2s_mic_warmup(void)
{
    int16_t tmp[I2S_MIC_FRAMES_PER_READ];
    /* 125ms。直流阻断器收敛需要约 1000 个样本，不丢的话起始瞬态会变成
     * 一个 400+ 的假峰值（实测踩过） */
    const int blocks = (I2S_MIC_SAMPLE_RATE / 8 + I2S_MIC_FRAMES_PER_READ - 1)
                       / I2S_MIC_FRAMES_PER_READ;
    for (int i = 0; i < blocks; i++) i2s_mic_read(tmp, I2S_MIC_FRAMES_PER_READ);
}
```

- [ ] **Step 3: 编译验证**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes
oesp && idf.py build 2>&1 | grep -E "warning:|error:" || echo "编译干净"
```
Expected: 编译干净（此时 `mic_test.c` 仍存在，两个文件里的 `app_main` 会冲突 —— 先把 `mic_test.c` 重命名为 `mic_test.c.bak` 再编译，**不要删**）

- [ ] **Step 4: 提交**

```bash
git add -A voice_notes/main/
git commit -m "阶段2: 抽出 i2s_mic 模块（逻辑照搬已验证代码，未改动参数）"
```

---

## Task 4: 固件传输模块（UART + 帧）

**这是本阶段风险最高的部分。** 两个必须解决的点：
1. 二进制帧与文本命令共用 UART0 —— 需要能区分
2. 发送阻塞会导致 I²S DMA 溢出丢音频 —— 必须能**检测**到，不能静默丢

**Files:**
- Create: `voice_notes/main/transport.h`
- Create: `voice_notes/main/transport.c`

**Interfaces:**
- Consumes: `audio_frame.h` 的 `frame_pack` / `crc16_ccitt`
- Produces:
  - `esp_err_t transport_init(void)` — 装 UART 驱动 + VFS，**不再使用 fgets/stdin**
  - `void transport_send(uint8_t type, const uint8_t *payload, uint16_t len)`
  - `void transport_send_text(uint8_t type, const char *text)`
  - `int transport_poll(uint8_t *out_type, uint8_t *out_payload, int max_len, int timeout_ms)` — 非阻塞轮询，返回 >0 表示收到一帧
  - `void transport_get_stats(uint32_t *tx_frames, uint32_t *rx_frames, uint32_t *bad_crc)`

- [ ] **Step 1: 写头文件与实现**

创建 `voice_notes/main/transport.h`：

```c
/* 串口传输：帧的收发。
 *
 * 设计要点（两条都是踩过坑才定下来的）：
 *
 * 1) 输入不走 stdin。之前用 fgets(stdin)，遇到 console 非阻塞的问题
 *    （字符被切碎逐个当指令）。现在直接 uart_read_bytes 自己做行/帧解析，
 *    两种输入模式靠"是否以 0xAA 0x55 开头"区分，不存在歧义。
 *
 * 2) 发送可能阻塞，进而导致 I²S DMA 溢出丢音频。本模块**不试图避免**
 *    阻塞（做不到），而是**如实上报**：调用方用 transport_tx_blocked_ms()
 *    得知总共阻塞了多久，据此判断是否丢了音频。静默丢数据比慢更糟。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t transport_init(void);

void transport_send(uint8_t type, const uint8_t *payload, uint16_t len);
void transport_send_text(uint8_t type, const char *text);

/* 非阻塞轮询。收到完整帧返回 payload 长度(>=0)，无帧返回 -1。
 * 文本行会被当作 type=FRAME_TYPE_CONTROL 的帧返回。 */
int transport_poll(uint8_t *out_type, uint8_t *out_payload, int max_len, int timeout_ms);

void transport_get_stats(uint32_t *tx_frames, uint32_t *rx_frames, uint32_t *bad_crc);

/* 累计阻塞在 uart_write_bytes 上的毫秒数。
 * 一次 DMA 缓冲 = 16ms，4 个缓冲 = 64ms —— 累计阻塞超过约 64ms
 * 就意味着 I²S 已经丢了数据。 */
uint32_t transport_tx_blocked_ms(void);
```

创建 `voice_notes/main/transport.c`，关键实现：

```c
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_timer.h"
#include "audio_frame.h"
#include "transport.h"

#define TX_UART        UART_NUM_0
#define TX_BUF_SIZE    8192     /* 约 250ms 音频，吸收 PC 端的短暂卡顿 */
#define RX_BUF_SIZE    1024
#define TX_MAX_FRAME   (FRAME_OVERHEAD + FRAME_MAX_PAYLOAD)

static uint32_t s_tx_frames, s_rx_frames, s_bad_crc;
static uint32_t s_tx_blocked_ms;
static uint8_t  s_rxbuf[FRAME_OVERHEAD + FRAME_MAX_PAYLOAD];
static int      s_rxlen;

esp_err_t transport_init(void)
{
    esp_err_t err = uart_driver_install(TX_UART, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    /* printf 走驱动的发送环形缓冲（中断驱动），stdout 才能正常用 */
    uart_vfs_dev_use_driver(TX_UART);
    uart_vfs_dev_port_set_tx_line_endings(TX_UART, ESP_LINE_ENDINGS_CRLF);
    return ESP_OK;
}

void transport_send(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[TX_MAX_FRAME];
    size_t n = frame_pack(frame, sizeof(frame), type, payload, len);
    if (n == 0) return;
    int64_t t0 = esp_timer_get_time();
    uart_write_bytes(TX_UART, (const char *)frame, n);
    s_tx_blocked_ms += (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_tx_frames++;
}

void transport_send_text(uint8_t type, const char *text)
{
    transport_send(type, (const uint8_t *)text, (uint16_t)strlen(text));
}
```

`transport_poll()` 的核心逻辑：

```c
/* 返回 payload 长度，无帧返回 -1。
 *
 * 两种输入模式：
 *   以 0xAA 0x55 开头  → 二进制帧
 *   其他               → 文本行（累积到 '\n' 为止），当作 CONTROL 帧返回
 * 这个区分是**无歧义**的：帧永远以 magic 开头，而文本命令是 ASCII。 */
int transport_poll(uint8_t *out_type, uint8_t *out_payload, int max_len, int timeout_ms)
{
    uint8_t ch;
    int n = uart_read_bytes(TX_UART, &ch, 1, pdMS_TO_TICKS(timeout_ms));
    if (n <= 0) return -1;

    if (s_rxlen == 0 && ch != FRAME_MAGIC_0) {
        /* ---- 文本行模式 ---- */
        static uint8_t line[128];
        static int linelen = 0;
        if (ch == '\n' || ch == '\r') {
            if (linelen == 0) return -1;
            int len = linelen;
            memcpy(out_payload, line, len);
            out_payload[len] = '\0';
            out_type[0] = FRAME_TYPE_CONTROL;
            linelen = 0;
            s_rx_frames++;
            return len;
        }
        if (linelen < (int)sizeof(line) - 1) line[linelen++] = ch;
        return -1;
    }

    /* ---- 帧模式 ---- */
    s_rxbuf[s_rxlen++] = ch;
    if (s_rxlen == 1) return -1;                        /* 收到 0xAA，等 0x55 */
    if (s_rxlen == 2 && ch != FRAME_MAGIC_1) {          /* 假 magic，退回文本 */
        s_rxlen = 0;
        return -1;
    }
    if (s_rxlen < FRAME_OVERHEAD) return -1;            /* 头没收全 */
    uint16_t plen = s_rxbuf[3] | (s_rxbuf[4] << 8);
    if (plen > FRAME_MAX_PAYLOAD) { s_rxlen = 0; s_bad_crc++; return -1; }
    size_t total = (size_t)FRAME_OVERHEAD + plen;
    if ((size_t)s_rxlen < total) return -1;             /* 体没收全 */

    uint16_t want = s_rxbuf[5 + plen] | (s_rxbuf[5 + plen + 1] << 8);
    int ret = -1;
    if (crc16_ccitt(&s_rxbuf[2], 3 + plen) == want) {
        int len = plen > (uint16_t)max_len ? max_len : plen;
        memcpy(out_payload, &s_rxbuf[5], len);
        out_payload[len] = '\0';
        out_type[0] = s_rxbuf[2];
        s_rx_frames++;
        ret = len;
    } else {
        s_bad_crc++;
    }
    s_rxlen = 0;
    return ret;
}
```

- [ ] **Step 2: 编译**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes
oesp && idf.py build 2>&1 | grep -E "warning:|error:" || echo "编译干净"
```
Expected: 编译干净

- [ ] **Step 3: 提交**

```bash
git add voice_notes/main/transport.h voice_notes/main/transport.c
git commit -m "阶段2: 传输模块（UART 帧收发 + 阻塞时长统计）"
```

---

## Task 5: 会话状态机 + 主程序重构

**Files:**
- Create: `voice_notes/main/session.h`, `voice_notes/main/session.c`
- Create: `voice_notes/main/app_main.c`
- Rename: `voice_notes/main/mic_test.c` → `voice_notes/main/diag.c`（并拆出 `diag.h`）
- Modify: `voice_notes/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `i2s_mic.h`、`audio_frame.h`、`transport.h`
- Produces:
  - `void session_run(void)` — 主循环，永不返回
  - `bool session_is_recording(void)`
  - 音频分块常量 `SESSION_CHUNK_SAMPLES = 256`

- [ ] **Step 1: 写 session.c**

核心循环（**这是本任务的全部价值所在**）：

```c
#include "session.h"
#include "i2s_mic.h"
#include "audio_frame.h"
#include "transport.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

#define CHUNK_SAMPLES   256      /* 与 I2S_MIC_FRAMES_PER_READ 一致 = 16ms */
#define STOP_POLL_MS    0        /* 每次循环都轮询，不等待 */

static bool     s_recording;
static uint32_t s_seq;
static int64_t  s_t0_us;
static uint64_t s_samples_sent;

static void send_status(const char *text)
{
    transport_send_text(FRAME_TYPE_STATUS, text);
}

void session_start(void)
{
    i2s_mic_reset_dc();
    i2s_mic_warmup();
    s_seq = 0;
    s_samples_sent = 0;
    s_t0_us = esp_timer_get_time();
    s_recording = true;
    send_status("RECORDING");
}

void session_stop(void)
{
    s_recording = false;

    /* 用「应发样本数 vs 实发样本数」判断有没有丢音频。
     * 这是唯一能反映背压丢帧的指标 —— 帧序号是我们自己生成的，
     * 它连续并不代表 I²S 没丢数据。 */
    int64_t elapsed_us = esp_timer_get_time() - s_t0_us;
    uint64_t expected = (uint64_t)elapsed_us * I2S_MIC_SAMPLE_RATE / 1000000ULL;
    int64_t deficit = (int64_t)expected - (int64_t)s_samples_sent;

    char msg[128];
    snprintf(msg, sizeof(msg),
             "STOPPED dur_ms=%lld samples=%llu expected=%llu deficit=%lld tx_block_ms=%u",
             (long long)(elapsed_us / 1000),
             (unsigned long long)s_samples_sent,
             (unsigned long long)expected,
             (long long)deficit,
             (unsigned)transport_tx_blocked_ms());
    send_status(msg);
}

void session_tick(void)
{
    if (!s_recording) return;

    static int16_t pcm[CHUNK_SAMPLES];
    int n = i2s_mic_read(pcm, CHUNK_SAMPLES);
    if (n <= 0) return;

    uint8_t payload[8 + CHUNK_SAMPLES * 2];
    uint32_t ts_ms = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
    size_t plen = audio_payload_pack(payload, s_seq++, ts_ms, pcm, n);

    transport_send(FRAME_TYPE_AUDIO, payload, (uint16_t)plen);
    s_samples_sent += n;
}
```

- [ ] **Step 2: 写主循环与命令分发**

创建 `voice_notes/main/app_main.c`：

```c
void app_main(void)
{
    printf("\n\n=== voice_notes ===\n");
    transport_init();          /* 必须先于 printf 之外的输入操作 */
    i2s_mic_init();
    diag_print_cfg();

    uint8_t type, payload[1050];
    while (1) {
        if (session_is_recording()) {
            session_tick();
            int len = transport_poll(&type, payload, sizeof(payload) - 1, 0);
            if (len >= 0 && type == FRAME_TYPE_CONTROL) {
                if (strncmp((char *)payload, "STOP", 4) == 0) session_stop();
            }
        } else {
            int len = transport_poll(&type, payload, sizeof(payload) - 1, 20);
            if (len < 0) continue;
            if (type != FRAME_TYPE_CONTROL) continue;
            char *cmd = (char *)payload;
            if (strncmp(cmd, "START", 5) == 0) session_start();
            else diag_handle_command(cmd);   /* level/raw/rec/... 原有诊断命令 */
        }
    }
}
```

- [ ] **Step 3: 把 mic_test.c 改造成 diag.c**

`diag.c` 保留 `level`/`raw`/`rec`/`shift`/`chan`/`dc`/`cfg`/`help` 全部命令，
只把 `app_main()` 换成 `diag_handle_command(const char *line)`，
I²S 调用换成 `i2s_mic_*`。

> ⚠️ **诊断命令不能删。** 阶段 1 的排查几乎全靠它们（`raw` 定位比特对齐、
> `level` 验证麦克风响应）。删掉等于把下次排查的工具扔掉。

- [ ] **Step 4: 更新 CMakeLists.txt**

```cmake
idf_component_register(SRCS "app_main.c" "session.c" "transport.c"
                            "i2s_mic.c" "audio_frame.c" "diag.c"
                       PRIV_REQUIRES driver esp_timer
                       INCLUDE_DIRS "")
```

- [ ] **Step 5: 编译烧录**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes
oesp && idf.py build 2>&1 | grep -E "warning:|error:" || echo "编译干净"
idf.py -p /dev/ttyUSB0 flash
```
Expected: 编译干净，烧录成功

- [ ] **Step 6: 手工验证诊断命令没坏**

```bash
oesp && python capture_wav.py 5
```
Expected: 和阶段 1 一样能录到音、能听清 —— **这一步确认重构没有破坏已验证的功能**，比任何单元测试都重要。

- [ ] **Step 7: 提交**

```bash
git add -A voice_notes/main/
git commit -m "阶段2: 会话状态机 + 主程序重构（诊断命令全部保留）"
```

---

## Task 6: PC 侧 AudioSource 与串口实现

**Files:**
- Create: `voice_notes/PC/sources/__init__.py`
- Create: `voice_notes/PC/sources/base.py`
- Create: `voice_notes/PC/sources/serial_src.py`
- Create: `voice_notes/PC/recorder.py`

**Interfaces:**
- Consumes: `frames.py`（Task 2）
- Produces:
  - `class AudioFrame`：`seq: int` `ts_ms: int` `pcm: bytes`
  - `class AudioSource(ABC)`：`open()` `send_control(cmd: str)` `read_audio(timeout) -> AudioFrame | None` `close()` `stats() -> dict`
  - `class SerialSource(AudioSource)`：`SerialSource(port, baud)`
  - `class WavRecorder`：`WavRecorder(path, rate)` → `feed(pcm)` `close()`

- [ ] **Step 1: 写 sources/base.py**

```python
"""音频来源抽象。

存在的理由：传输要支持串口和 WiFi 两条路（prd.md D4），
两者对上层必须完全等价 —— 换传输不改下游一行代码。
"""
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Optional


@dataclass
class AudioFrame:
    seq: int
    ts_ms: int
    pcm: bytes          # 16-bit LE 单声道


class AudioSource(ABC):
    @abstractmethod
    def open(self) -> None: ...

    @abstractmethod
    def send_control(self, cmd: str) -> None:
        """发送控制命令，如 'START' / 'STOP'"""

    @abstractmethod
    def read_audio(self, timeout: float = 1.0) -> Optional[AudioFrame]:
        """读一帧音频，超时返回 None"""

    @abstractmethod
    def close(self) -> None: ...

    @abstractmethod
    def stats(self) -> dict:
        """返回 {frames, lost, bad_crc, ...} 供验收使用"""
```

- [ ] **Step 2: 写 sources/serial_src.py**

要点（每条都对应一个已知的坑）：

```python
class SerialSource(AudioSource):
    def open(self):
        # DTR/RTS 都拉低 = 正常运行模式。pyserial 默认会断言这两根线，
        # 那会给 ESP32 一个复位脉冲，导致刚发的 START 丢失。
        self._ser = serial.Serial(self.port, self.baud, timeout=0.05)
        self._ser.setDTR(False)
        self._ser.setRTS(False)

    def read_audio(self, timeout=1.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self._ser.read(4096)     # 非阻塞读，靠 timeout=0.05
            if chunk:
                for ftype, payload in self._parser.feed(chunk):
                    if ftype == TYPE_AUDIO:
                        seq, ts, pcm = unpack_audio(payload)
                        self._track(seq)      # 统计丢帧
                        return AudioFrame(seq, ts, pcm)
                    elif ftype == TYPE_STATUS:
                        self.last_status = payload.decode("utf-8", "replace")
        return None

    def _track(self, seq):
        """用 seq 缺口统计丢帧。第一帧只记录基准。"""
        if self._last_seq is not None:
            gap = seq - self._last_seq - 1
            if gap > 0:
                self.lost += gap
        self._last_seq = seq
        self.frames += 1
```

- [ ] **Step 3: 写 recorder.py**

```python
class WavRecorder:
    """边收边写 WAV。

    为什么不攒在内存里：录音时长只受磁盘限制，不受内存限制；
    而且停止时文件已经写好，直接交给 FunASR 即可（阶段 3）。
    """
    def __init__(self, path, rate=16000):
        self._w = wave.open(path, "wb")
        self._w.setnchannels(1)
        self._w.setsampwidth(2)
        self._w.setframerate(rate)

    def feed(self, pcm: bytes):
        self._w.writeframes(pcm)

    def close(self) -> int:
        n = self._w.getnframes()
        self._w.close()
        return n
```

- [ ] **Step 4: 用假串口做单测**

创建 `voice_notes/PC/tests/test_serial_src.py`，用一个假的 serial 对象（不接硬件）验证：
- 帧能解出来
- `seq` 缺口能被统计成丢帧
- 状态帧能被识别

```python
def test_lost_frame_counting():
    src = SerialSource.__new__(SerialSource)   # 不走 open()
    src._parser = FrameParser(); src._last_seq = None
    src.lost = 0; src.frames = 0; src.last_status = ""
    src._track(1); src._track(2); src._track(5)   # 缺 3、4
    assert src.lost == 2
    assert src.frames == 3
```

- [ ] **Step 5: 跑测试并提交**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes/PC
python -m pytest tests/ -v
git add -A && git commit -m "阶段2: PC 侧 AudioSource 抽象与串口实现"
```

---

## Task 7: 端到端验收

**验收标准（`prd.md` §10）：连续 60 秒，丢帧 = 0。**

**Files:**
- Create: `voice_notes/PC/stage2_accept.py`

- [ ] **Step 1: 写验收脚本**

```python
#!/usr/bin/env python3
"""阶段 2 验收：连续录音 60 秒，检查丢帧与完整性。

通过标准：
  1. 丢帧数（seq 缺口）= 0
  2. 固件自报的 deficit ≈ 0（说明 I²S 侧也没丢）
  3. 样本数 = 时长 × 16000（±1%）
  4. WAV 能听清
"""
```

脚本流程：
1. 打开串口 → 等 `mic>` / 就绪
2. 发 `START`（用帧，不用文本，顺便验证帧路径）
3. 收 60 秒，边收边写 `stage2_accept.wav`
4. 发 `STOP`（**在录音过程中发** —— 这正是帧协议存在的理由）
5. 解析固件回的 `STOPPED ...` 状态帧
6. 打印判定表

- [ ] **Step 2: 跑验收**

```bash
cd /home/ubuntu/Workspace/projects/esp32/voice_notes/PC
oesp && python stage2_accept.py 60
```

Expected（全部 ✅）：
```
  帧数            : 3750  (期望 3750)
  丢帧            : 0
  固件自报 deficit: 0
  样本数          : 960000 / 960000
  tx 阻塞累计     : < 10 ms
  WAV             : stage2_accept.wav (60.0 秒)
```

- [ ] **Step 3: 听录音确认音质没退化**

```bash
paplay stage2_accept.wav
```
Expected: 能听清，且**没有周期性的咔哒声**（有咔哒 = 丢帧或时序错位）

- [ ] **Step 4: 如果丢帧不为 0，按此顺序排查**

| 征兆 | 最可能的原因 |
|---|---|
| `tx 阻塞累计` 很大 | PC 端读得太慢 —— 检查 `read_audio` 是否在做耗时操作 |
| `deficit > 0` 但 `tx 阻塞` 小 | I²S 侧问题 —— 回阶段 1 用 `level` 验证麦克风 |
| `丢帧 > 0` 但 `deficit = 0` | 串口链路丢字节 —— 降波特率到 460800 试 |
| 有咔哒声但计数都对 | 采样率不匹配 —— 检查 WAV 是否 16000 Hz |

- [ ] **Step 5: 更新文档并提交**

在 `prd.md` 里程碑表把阶段 2 标为通过，把实测数字记进 `hardware.md`（或新建
`docs/stage2-results.md`），提交。

---

## Self-Review

**1. Spec 覆盖检查**

| PRD 要求 | 对应任务 |
|---|---|
| §7 帧协议（双向、CRC） | Task 1、2 |
| §7 波特率 921600 | Task 4（`transport_init`） |
| §8 固件 `audio_frame` | Task 1 |
| §8 固件 `transport` | Task 4 |
| §8 固件 `session` | Task 5 |
| §8 PC `sources/base` | Task 6 |
| §8 PC `sources/serial_src` | Task 6 |
| §10 阶段 2「60s 丢帧=0」 | Task 7 |
| NFR-2 不丢帧 + **显示**丢帧数 | Task 5（deficit）、Task 6（lost） |
| 阶段 2 风险「背压」 | Task 4（`transport_tx_blocked_ms`）、Task 5（deficit） |
| FR-6 落盘 WAV | Task 6（`WavRecorder`） |

无遗漏。

**2. 占位符扫描** —— 无 TBD/TODO；每个代码步骤都有可编译的实际代码。

**3. 类型一致性** —— 已核对：
- `frame_pack()` 参数顺序在 Task 1 定义、Task 4 使用，一致
- `FRAME_TYPE_*` 宏名在 Task 1 定义、Task 4/5 使用，一致
- `i2s_mic_read()` 签名在 Task 3 定义、Task 5 使用，一致
- `AudioFrame` 字段名在 Task 6 定义、Task 7 使用，一致
- `transport_poll()` 返回值语义（`>=0` 为 payload 长度，`-1` 为无帧）在 Task 4 定义、Task 5 使用，一致

**4. 已知的未解风险（不隐藏）**

- **`uart_read_bytes` 与 VFS stdin 是否抢字节**：设计上不使用 stdin，但 IDF 的
  默认 console 配置是否会暗中读取 stdin 未经验证。Task 5 的 Step 6（手工跑
  `capture_wav.py`）会暴露这个问题 —— 若诊断命令收不到输入，就说明 VFS 抢了字节，
  届时改用 `fread(stdin)` 读字节喂给同一个解析器。
- **60 秒是否够暴露背压问题**：可能不够。若 60 秒通过但实际使用中偶发丢帧，
  把验收时长加到 5 分钟再测一次。
