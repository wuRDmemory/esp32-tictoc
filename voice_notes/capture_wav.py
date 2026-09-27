#!/usr/bin/env python3
"""
阶段 1 验证 · PC 侧：让板子录一段音，存成 WAV

用法:
    oesp && python capture_wav.py [秒数] [输出文件]

对应 docs/hardware.md §4「第 3 级：听感」—— 这是阶段 1 的最终验收。
前面两级（时钟对不对、数据合不合理）用板子上的 `rec` 命令做。

注意: 串口独占。跑之前先 Ctrl+] 退出 idf.py monitor，否则报
      "multiple access on port"。
"""
import math
import struct
import sys
import time
import wave

import serial

PORT = "/dev/ttyUSB0"
BAUD = 921600


def read_until(ser, marker: bytes, timeout_s=10.0):
    """读到 marker 出现为止，返回 (marker 所在行, marker 之后的字节)"""
    buf = b""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        n = ser.in_waiting
        chunk = ser.read(n if n else 1)
        if not chunk:
            continue
        buf += chunk
        i = buf.find(marker)
        if i < 0:
            # 防止无限增长
            if len(buf) > 8192:
                buf = buf[-1024:]
            continue
        j = buf.find(b"\n", i)
        if j < 0:
            continue
        line = buf[i:j].decode("ascii", "replace").strip()
        return line, buf[j + 1:]
    return None, b""


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    outfile = sys.argv[2] if len(sys.argv) > 2 else "mic_test.wav"

    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
    except serial.SerialException as e:
        print(f"❌ 打不开 {PORT}: {e}")
        print("   1) 设备在不在: ls -la /dev/ttyUSB0")
        print("   2) 权限对不对: sudo chmod 666 /dev/ttyUSB0")
        print("   3) 监视器占着口没: Ctrl+] 退出 idf.py monitor")
        return 1

    # DTR→GPIO0, RTS→EN。都拉低 = 正常运行模式（不复位、不进下载模式）
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.3)

    # 等板子启动完（打开串口本身会产生一次复位脉冲）
    print("等待板子启动...", end="", flush=True)
    boot, _ = read_until(ser, b"mic>", timeout_s=8)
    if boot is None:
        print("\n⚠️  没等到提示符，仍然尝试发命令")
    else:
        print(" 就绪")

    ser.reset_input_buffer()
    cmd = f"wav {seconds}\n"
    print(f">>> 发送: {cmd.strip()}")
    ser.write(cmd.encode())

    header, payload = read_until(ser, b"WAV_BEGIN", timeout_s=10)
    if header is None:
        print("❌ 没收到 WAV_BEGIN。板子可能没在跑这个固件，或者波特率不对。")
        ser.close()
        return 1

    parts = header.split()
    if len(parts) != 3:
        print(f"❌ 头部格式异常: {header!r}")
        ser.close()
        return 1
    total, rate = int(parts[1]), int(parts[2])
    need = total * 2
    print(f">>> 板子在录 {total} 个样本 @ {rate} Hz（{total / rate:.1f} 秒，{need} 字节）")

    pcm = bytearray(payload[:need])
    t0 = time.time()
    while len(pcm) < need:
        chunk = ser.read(min(65536, need - len(pcm)))
        if not chunk:
            if time.time() - t0 > seconds * 3 + 15:
                print(f"⚠️  接收超时，只拿到 {len(pcm)}/{need} 字节")
                break
            continue
        pcm += chunk
    elapsed = time.time() - t0

    tail, _ = read_until(ser, b"WAV_END", timeout_s=5)
    ser.close()

    if tail is None:
        print("⚠️  没收到 WAV_END（数据可能截断）")

    got = len(pcm) // 2
    if got == 0:
        print("❌ 一个样本都没收到")
        return 1

    # 写 WAV
    with wave.open(outfile, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(pcm[: got * 2]))

    # 统计
    samples = struct.unpack(f"<{got}h", bytes(pcm[: got * 2]))
    vmin, vmax = min(samples), max(samples)
    peak = max(abs(vmin), abs(vmax))
    rms = (sum(s * s for s in samples) / got) ** 0.5
    quiet = sum(1 for s in samples if -100 < s < 100) / got * 100

    print(f"\n================ 接收统计 ================")
    print(f"样本数     : {got} / {total}")
    print(f"用时       : {elapsed:.1f} 秒  (音频时长 {got / rate:.1f} 秒)")
    print(f"有效速率   : {got * 2 / elapsed / 1024:.1f} KB/s  (PCM 净速率应为 31.2)")
    print(f"最小值/最大值: {vmin} / {vmax}")
    dbfs = 20 * math.log10(peak / 32768.0) if peak > 0 else -999.0
    print(f"峰值       : {peak}  ({dbfs:.1f} dBFS)")
    print(f"RMS        : {rms:.1f}")
    print(f"静音样本比 : {quiet:.1f} %")
    print(f"==========================================")
    print(f"\n🎧 存好了: {outfile}")
    print(f"   播放听听能不能听清自己说话 —— 这是阶段 1 的最终验收")
    print(f"   WSL 里可以试试: cp {outfile} /mnt/c/Users/ && 然后在 Windows 里播")
    return 0


if __name__ == "__main__":
    sys.exit(main())
