#!/usr/bin/env python3
"""
ESP32-PICO-KIT V4 端到端验证脚本

做三件事:
  1. 用 DTR/RTS 硬复位板子, 抓完整启动日志 (验证 Flash 配置是否生效)
  2. 发一行文本, 验证板子能收 (RX) 且能回 (TX)  -> 双向通信
  3. 发 info 命令, 验证命令解析

注意: DTR 接 GPIO0(启动模式), RTS 接 EN(复位), 这是 ESP32 的经典自动复位电路。
      不要用 esptool 之外的方式复位, 否则可能进不了正常启动模式。
"""
import sys
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200


def drain(ser, seconds, label):
    """读 seconds 秒, 边读边打印"""
    print(f"\n--- {label} ---")
    t0 = time.time()
    buf = b""
    while time.time() - t0 < seconds:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
    text = buf.decode("utf-8", "replace")
    sys.stdout.write(text)
    sys.stdout.flush()
    return text


def main():
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.5)
    except serial.SerialException as e:
        print(f"❌ 打不开 {PORT}: {e}")
        print("   检查: 1) 设备是否存在  2) 权限是否 chmod 666")
        return 1

    print(f"✅ 已打开 {PORT} @ {BAUD}")
    ser.reset_input_buffer()

    # ---- 硬复位, 抓启动日志 ----
    ser.setDTR(False)      # GPIO0 = HIGH -> 正常启动模式(不进下载模式)
    ser.setRTS(True)       # EN = LOW     -> 按住复位
    time.sleep(0.15)
    ser.setRTS(False)      # EN = HIGH    -> 释放复位, 板子开始启动
    time.sleep(0.1)
    ser.reset_input_buffer()

    boot = drain(ser, 6, "启动日志")

    # ---- 测试 1: 回显 (双向通信) ----
    probe = "hello-esp32-pico"
    print(f"\n>>> 发送: {probe!r}")
    ser.write((probe + "\n").encode())
    time.sleep(1.5)
    echo = drain(ser, 1, "板子回应")

    # ---- 测试 2: info 命令 ----
    print("\n>>> 发送: 'info'")
    ser.write(b"info\n")
    time.sleep(2.5)
    info = drain(ser, 1, "info 命令回应")

    ser.close()

    # ---- 判定 ----
    print("\n" + "=" * 46)
    print("判定")
    print("=" * 46)

    checks = [
        ("芯片型号是 esp32",        "芯片型号   : esp32" in boot),
        ("Flash 报告 8 MB",         "Flash 容量 : 8 MB" in boot),
        ("PSRAM 报告 2 MB",         "PSRAM 容量 : 2 MB" in boot),
        ("PSRAM 已启用(非未启用)",   "PSRAM 容量 : 未启用" not in boot),
        ("回显内容正确",             probe in echo),
        ("info 命令有响应",          "板子信息" in info),
    ]

    all_ok = True
    for name, ok in checks:
        print(f"  {'✅' if ok else '❌'} {name}")
        if not ok:
            all_ok = False

    print("=" * 46)
    print("🎉 全部通过 — 双向通信链路已验证" if all_ok else "⚠️ 有项目未通过, 见上方输出")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
