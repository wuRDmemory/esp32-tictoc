#!/usr/bin/env python3
"""
小智固件串口日志捕获 —— ESP32-S3-CAM (USB-Serial-JTAG)

为什么不用 `idf.py monitor`：
  它是交互式的，脚本里抓不干净；而这个脚本可以在**打开端口之后**再复位板子，
  这样从第一行 ROM 日志起就不会漏（先复位再开端口必然丢开头，而 panic 和
  激活码恰恰都在开头）。

为什么 DTR/RTS 能复位 USB-Serial-JTAG：
  该外设把 CDC 的 DTR/RTS 映射成和传统 USB-UART 桥一样的 GPIO0/EN，
  所以 esptool 的默认复位时序在这里同样成立。
    RTS=1 → EN 拉低（复位保持）
    DTR=1 → GPIO0 拉低（下载模式；正常运行要放掉）

用法：
    python xiaozhi_monitor.py                # 复位并抓 60 秒
    python xiaozhi_monitor.py -t 120         # 抓 120 秒
    python xiaozhi_monitor.py --no-reset     # 不复位，只监听（板子刚跑起来时用）
    python xiaozhi_monitor.py -p /dev/ttyACM0
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial：oesp && pip install pyserial")

# 抓到这个就提前收工（说明已经走到我们关心的那一步）
STOP_MARKERS = ("ACTIVATION CODE",)


def default_port():
    """S3-CAM 走原生 USB → ttyACM*；经典 ESP32 走 CP2102N → ttyUSB*。"""
    import glob
    for pat in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


def reset_into_app(ser):
    """拉 EN 复位，然后放开 —— 让板子从 ROM 开始重新跑。"""
    ser.setDTR(False)   # GPIO0 高 = 正常运行，不进下载模式
    ser.setRTS(True)    # EN 低 = 保持复位
    time.sleep(0.12)
    ser.setRTS(False)   # 放开复位
    time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None)
    ap.add_argument("-t", "--timeout", type=float, default=60.0)
    ap.add_argument("--no-reset", action="store_true",
                    help="不复位，直接监听当前输出")
    args = ap.parse_args()

    port = args.port or default_port()
    if not port:
        sys.exit("找不到串口设备（/dev/ttyACM* 或 /dev/ttyUSB*）")

    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"打不开 {port}: {e}\n"
                 f"  → 端口被占用？先退出 idf.py monitor\n"
                 f"  → 权限不足？sudo chmod 666 {port}")

    print(f"[monitor] {port} @115200，抓取 {args.timeout:.0f} 秒"
          f"{'（先复位）' if not args.no_reset else '（不复位）'}", flush=True)
    print("-" * 70, flush=True)

    if not args.no_reset:
        reset_into_app(ser)
        ser.reset_input_buffer()

    deadline = time.time() + args.timeout
    reconnects = 0
    try:
        while time.time() < deadline:
            try:
                line = ser.readline()
            except (serial.SerialException, OSError) as e:
                # 配网后芯片会重启，USB-Serial-JTAG 的 CDC 可能瞬断。
                # 这里必须重连而不是退出 —— 否则激活码恰好就在断开的那几秒里丢掉。
                reconnects += 1
                if reconnects > 20:
                    print(f"\n[monitor] 重连 {reconnects} 次仍失败，放弃: {e}")
                    break
                print(f"\n[monitor] 串口断开（第 {reconnects} 次），重连中…", flush=True)
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(1.0)
                try:
                    ser = serial.Serial(port, 115200, timeout=0.2)
                    print("[monitor] 已重连", flush=True)
                except serial.SerialException:
                    time.sleep(1.0)
                continue
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip("\r\n")
            print(text, flush=True)
            if any(m in text for m in STOP_MARKERS):
                print("-" * 70)
                print("[monitor] 已抓到激活码，提前结束")
                break
    except KeyboardInterrupt:
        print("\n[monitor] 用户中断")
    finally:
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
