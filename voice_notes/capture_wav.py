#!/usr/bin/env python3
"""
阶段 1 验收工具：录一段音存成 WAV，并当场分析

用法:
    oesp && python capture_wav.py [秒数] [输出文件]

为什么要有倒计时：之前是"脚本一发命令就开始录"，人来不及反应，导致
连续几次录到的都是静音，看起来像麦克风坏了 —— 其实是配合问题。
现在有 3 秒倒计时，节奏由你掌握。

注意: 串口独占。先 Ctrl+] 退出 idf.py monitor。
"""
import math
import struct
import sys
import time
import wave

import serial

PORT = "/dev/ttyUSB0"
BAUD = 921600


def send_slow(ser, text, per_char=0.05):
    """逐字符发送。手敲的速度才是真实场景，一次性发整行会掩盖缓冲问题。"""
    for ch in text:
        ser.write(ch.encode())
        time.sleep(per_char)
    ser.write(b"\n")


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    outfile = sys.argv[2] if len(sys.argv) > 2 else "mic_test.wav"

    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
    except serial.SerialException as e:
        print(f"❌ 打不开 {PORT}: {e}")
        print("   1) 设备在不在:  ls -la /dev/ttyUSB0")
        print("   2) 权限对不对:  sudo chmod 666 /dev/ttyUSB0")
        print("   3) 监视器占着没: lsof /dev/ttyUSB0   （Ctrl+] 退出 idf.py monitor）")
        return 1

    try:
        ser.setDTR(False)
        ser.setRTS(False)
        time.sleep(0.3)

        print("连接板子...", end="", flush=True)
        t0 = time.time()
        ready = False
        while time.time() - t0 < 8:
            if b"mic> " in ser.read(65536):
                ready = True
                break
        print(" 就绪" if ready else " (没等到提示符，仍然继续)")
        ser.reset_input_buffer()

        # ---- 倒计时：节奏交给你 ----
        print()
        print("=" * 58)
        print(f"  马上录 {seconds} 秒。请把嘴凑到麦克风 20cm 以内！")
        print("  倒计时结束后就开始说话（随便说什么，持续说）。")
        print("=" * 58)
        for i in (3, 2, 1):
            print(f"    {i} ...")
            time.sleep(1)
        print("    ▶▶ 开始说话！◀◀")
        send_slow(ser, f"wav {seconds}")

        # ---- 收 WAV_BEGIN ----
        buf = b""
        deadline = time.time() + 12
        hdr = None
        payload = b""
        while time.time() < deadline:
            c = ser.read(65536)
            if c:
                buf += c
            i = buf.find(b"WAV_BEGIN")
            if i >= 0:
                j = buf.find(b"\n", i)
                if j > 0:
                    hdr = buf[i:j].decode()
                    payload = buf[j + 1:]
                    break
        if not hdr:
            print("❌ 没收到 WAV_BEGIN。板子在跑这个固件吗？")
            return 1

        parts = hdr.split()
        total, rate = int(parts[1]), int(parts[2])
        need = total * 2

        pcm = bytearray(payload[:need])
        t0 = time.time()
        while len(pcm) < need:
            c = ser.read(min(65536, need - len(pcm)))
            if c:
                pcm += c
            elif time.time() - t0 > seconds * 3 + 15:
                print(f"⚠️  接收超时，只拿到 {len(pcm)}/{need} 字节")
                break
        ser.read(4096)   # 收尾
    finally:
        ser.close()

    got = len(pcm) // 2
    if got == 0:
        print("❌ 一个样本都没收到")
        return 1

    with wave.open(outfile, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(pcm[: got * 2]))

    # ---- 当场分析：按 0.5 秒切块看包络 ----
    s = struct.unpack(f"<{got}h", bytes(pcm[: got * 2]))
    blk = rate // 2
    print(f"\n录到 {got} 样本 = {got / rate:.1f} 秒 @ {rate} Hz\n")
    print("  时间    电平      包络")
    levels = []
    for i in range(0, max(1, got - blk), blk):
        seg = s[i:i + blk]
        rms = math.sqrt(sum(v * v for v in seg) / len(seg))
        db = 20 * math.log10(rms / 32768) if rms else -99.0
        levels.append(db)
        bar = "#" * max(0, int((db + 80) / 80 * 40))
        print(f"  {i / rate:5.1f}s {db:6.1f} dBFS {bar}")

    peak = max(abs(v) for v in s)
    span = max(levels) - min(levels) if levels else 0
    print(f"\n峰值 {peak} ({20 * math.log10(peak / 32768) if peak else -99:.1f} dBFS)"
          f"   包络动态范围 {span:.1f} dB")
    if span > 12:
        print("✅ 有高低起伏 —— 录到语音了")
    elif max(levels) > -45:
        print("⚠️  有声音但偏平 —— 说话再近一点、大声一点")
    else:
        print("❌ 全程平稳 —— 这段里没说话，或者离得太远")

    print(f"\n🎧 播放听听（WSLg 已就绪）:")
    print(f"     paplay {outfile}")
    print(f"   或拷到 Windows:")
    print(f"     cp {outfile} /mnt/c/Users/$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\\r')/Desktop/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
