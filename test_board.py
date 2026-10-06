#!/usr/bin/env python3
"""
ESP32 端到端验证脚本（hello_world 固件）

做三件事:
  1. 用 DTR/RTS 硬复位板子, 抓完整启动日志 (验证 Flash/PSRAM 配置是否生效)
  2. 发一行文本, 验证板子能收 (RX) 且能回 (TX)  -> 双向通信
  3. 发 info 命令, 验证命令解析

⚠️ 断言是**按芯片**取预期值的（见 EXPECTED），不要写死。
   两台板子的 Flash/PSRAM 完全不同：
     S3-CAM  : esp32s3 / 16 MB / 8 MB   ← 当前唯一目标
     PICO    : esp32   /  8 MB / 2 MB   ← 2026-10-04 起已停止验证（D14）

⚠️ 端口是**自动探测**的（S3 优先），不要再写死。
   S3 走原生 USB → /dev/ttyACM0；PICO 走 CP2102N → /dev/ttyUSB0。

注意: DTR 接 GPIO0(启动模式), RTS 接 EN(复位), 这是 ESP32 的经典自动复位电路。
      不要用 esptool 之外的方式复位, 否则可能进不了正常启动模式。
"""
import argparse
import glob
import re
import sys
import time

import serial

BAUD = 115200

# 各芯片的预期值。键是 hello_world 打印的 CONFIG_IDF_TARGET。
# 加新板子时在这里加一行，而不是去改断言本身。
EXPECTED = {
    "esp32":   {"name": "ESP32-PICO-V3-02", "flash_mb": 8,  "psram_mb": 2},
    "esp32s3": {"name": "ESP32-S3-CAM",     "flash_mb": 16, "psram_mb": 8},
}


def default_port():
    """S3 走原生 USB(ttyACM)，PICO 走 CP2102N(ttyUSB)。S3 优先。"""
    for pat in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


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


def parse_boot(boot):
    """从启动日志里抽出实际值。抽不到的返回 None。"""
    def grab(pattern, cast=str):
        m = re.search(pattern, boot)
        return cast(m.group(1)) if m else None

    return {
        "chip":     grab(r"芯片型号\s*:\s*(\S+)"),
        "flash_mb": grab(r"Flash 容量\s*:\s*(\d+)\s*MB", int),
        "psram_mb": grab(r"PSRAM 容量\s*:\s*(\d+)\s*MB", int),
        # 板子会打印 "PSRAM 容量 : 未启用" 而不是数值
        "psram_off": "PSRAM 容量 : 未启用" in boot,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None,
                    help="串口设备；默认自动探测（S3 优先）")
    args = ap.parse_args()

    port = args.port or default_port()
    if not port:
        print("❌ 找不到串口设备（/dev/ttyACM* 或 /dev/ttyUSB*）")
        return 1

    try:
        ser = serial.Serial(port, BAUD, timeout=0.5)
    except serial.SerialException as e:
        print(f"❌ 打不开 {port}: {e}")
        print("   检查: 1) 设备是否存在  2) 权限是否 chmod 666")
        print("   提示: S3 是 /dev/ttyACM0，PICO 才是 /dev/ttyUSB0")
        return 1

    print(f"✅ 已打开 {port} @ {BAUD}")
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
    probe = "hello-esp32-echo"
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
    got = parse_boot(boot)
    exp = EXPECTED.get(got["chip"])

    print("\n" + "=" * 46)
    print("判定")
    print("=" * 46)
    print(f"  实测: 芯片={got['chip']} Flash={got['flash_mb']}MB "
          f"PSRAM={'未启用' if got['psram_off'] else str(got['psram_mb']) + 'MB'}")

    if exp is None:
        print(f"  ⚠️ 未知芯片 {got['chip']!r} —— 跳过板级断言（在 EXPECTED 里加一行即可）")
        checks = []
    else:
        print(f"  预期({exp['name']}): Flash={exp['flash_mb']}MB PSRAM={exp['psram_mb']}MB")
        checks = [
            ("Flash 容量与预期一致", got["flash_mb"] == exp["flash_mb"]),
            ("PSRAM 已启用",         not got["psram_off"]),
            ("PSRAM 容量与预期一致", got["psram_mb"] == exp["psram_mb"]),
        ]

    # 与板子无关的通用断言
    checks += [
        ("回显内容正确",  probe in echo),
        ("info 命令有响应", "板子信息" in info),
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
