#!/usr/bin/env python3
"""阶段 2 验收：连续录音 N 秒，检查丢帧与完整性。

用法：
    cd voice_notes/PC && oesp && python stage2_accept.py [秒数]

通过标准（四条全过才算过）：
  1. 丢帧（seq 缺口）= 0                  —— 固件发出的帧都到了
  2. 固件自报 deficit ≈ 0                 —— I²S 侧也没丢样本  ← 这条最关键
  3. 样本数 ≈ 时长 × 16000（±1%）         —— 端到端一致
  4. WAV 能听清且无周期性咔哒声            —— 需人耳确认

⚠️ 第 2 条是第 1 条的必要补充：seq 是固件自己生成的，它连续只说明
   "发出的帧都到了"。I²S 漏采的样本根本不会变成帧，只有"按时间应该
   产出多少 vs 实际发出多少"的差值（deficit）能暴露。

注意：串口独占。先 Ctrl+] 退出 idf.py monitor，或确认 lsof 没占用。
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from frames import TYPE_STATUS  # noqa: E402
from recorder import WavRecorder  # noqa: E402
from sources.serial_src import SerialSource  # noqa: E402

RATE = 16000


def parse_status(s: str) -> dict:
    """解析固件回的 STOPPED 状态帧"""
    out = {}
    for tok in s.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                out[k] = v
    return out


def main() -> int:
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    wav_path = Path(__file__).resolve().parent / "stage2_accept.wav"

    src = SerialSource()
    try:
        src.open()
    except Exception as e:  # noqa: BLE001
        print(f"❌ 打不开串口: {e}")
        print("   1) 设备在不在:  ls -la /dev/ttyUSB0")
        print("   2) 权限对不对:  sudo chmod 666 /dev/ttyUSB0")
        print("   3) 谁占着口:    lsof /dev/ttyUSB0")
        return 1

    try:
        print("等待板子就绪...", end="", flush=True)
        print(" 就绪" if src.wait_ready(10) else " (没等到，继续)")

        rec = WavRecorder(wav_path, RATE)
        print(f">>> 发送 START，录音 {seconds} 秒")
        src.send_control("START")

        deadline = time.time() + seconds
        frames_seen = 0
        while time.time() < deadline:
            fr = src.read_audio(timeout=min(1.0, max(0.05, deadline - time.time())))
            if fr is not None:
                rec.feed(fr.pcm)
                frames_seen += 1

        # 关键：STOP 是在**录音过程中**发的，这正是帧协议存在的理由
        print(">>> 录音结束，发送 STOP")
        src.send_control("STOP")
        src.drain(1.5)          # 排空尾帧，顺便收 STOPPED 状态

        samples = rec.close()
        st = src.stats()
        status = parse_status(st["last_status"])
    finally:
        src.close()

    # ---- 判定 ----
    expect_samples = seconds * RATE
    deficit = status.get("deficit")
    tx_block = status.get("tx_block_ms")

    checks = [
        ("丢帧 = 0（帧序号无缺口）", st["lost"] == 0, f"lost={st['lost']}"),
        ("I²S 无丢样本（deficit≈0）", deficit is not None and abs(deficit) < 1000,
         f"deficit={deficit}"),
        ("样本数符合时长（±1%）", abs(samples - expect_samples) <= expect_samples * 0.01,
         f"{samples}/{expect_samples}"),
        ("无 CRC 错误", st["bad_crc"] == 0, f"bad_crc={st['bad_crc']}"),
        ("TX 阻塞轻微（<64ms）", tx_block is not None and tx_block < 64,
         f"tx_block_ms={tx_block}"),
    ]

    print()
    print("=" * 52)
    print("阶段 2 验收判定")
    print("=" * 52)
    ok_all = True
    for name, ok, detail in checks:
        print(f"  {'✅' if ok else '❌'} {name:28} {detail}")
        ok_all = ok_all and ok
    print("=" * 52)
    print(f"  帧数 {st['frames']}  时长 {samples / RATE:.1f}s")
    print(f"  固件状态: {st['last_status'] or '(未收到)'}")
    print(f"  WAV: {wav_path}")
    print()

    if ok_all:
        print("🎉 全部通过。最后一步：")
        print(f"     paplay {wav_path}")
        print("   听是否清晰、有无周期性咔哒声（有咔哒 = 丢帧或时序错位）")
    else:
        print("⚠️ 有未通过项，排查顺序见 plan 的 Task 7 Step 4")

    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
