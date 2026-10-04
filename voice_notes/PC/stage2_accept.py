#!/usr/bin/env python3
"""阶段 2 验收：连续录音 N 秒，检查丢帧与完整性。

用法：
    cd voice_notes/PC && oesp && python stage2_accept.py [秒数] [串口]

    python stage2_accept.py 60                  # 自动挑口（S3 优先）
    python stage2_accept.py 60 /dev/ttyUSB0     # 显式指定（多板同时插着时用）

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
    # 第二个参数是串口；不给就自动挑（S3 优先，见 serial_src.default_port）
    port = sys.argv[2] if len(sys.argv) > 2 else None
    wav_path = Path(__file__).resolve().parent / "stage2_accept.wav"

    src = SerialSource(port=port)
    try:
        src.open()
    except Exception as e:  # noqa: BLE001
        print(f"❌ 打不开串口 {src.port}: {e}")
        print(f"   1) 设备在不在:  ls -la {src.port}")
        print(f"   2) 权限对不对:  sudo chmod 666 {src.port}")
        print(f"   3) 谁占着口:    lsof {src.port}")
        return 1

    # ⚠️ 把实际用的口打出来 —— 双板环境下不能让"用的是哪块板"成为隐藏状态
    print(f"串口: {src.port}")

    try:
        print("等待板子就绪...", end="", flush=True)
        print(" 就绪" if src.wait_ready(10) else " (没等到，继续)")

        rec = WavRecorder(wav_path, RATE)
        print(f">>> 发送 START，录音 {seconds} 秒")
        src.send_control("START")

        deadline = time.time() + seconds
        frames_seen = 0
        t_start = time.time()
        # 记录每次丢帧/CRC 错误发生的时刻与数量。
        # 用途：区分"聚集在开头"（启动残留/monitor 遗留）与"均匀分布"（真·链路噪声），
        # 这两者的修法完全不同。
        events = []
        last_lost = last_bad = 0

        self_stopped = False
        while time.time() < deadline:
            fr = src.read_audio(timeout=min(1.0, max(0.05, deadline - time.time())))
            if fr is not None:
                rec.feed(fr.pcm)
                frames_seen += 1

            # ⚠️ D13 之后固件可能**自己判停**（S3 上的板端 VAD，`by=VAD`）。
            #    收到 STOPPED 就该收工，不能傻等到计划的秒数 ——
            #    否则会把"固件正常自停"误报成"脚本没录满"。
            if src.last_status.startswith("STOPPED"):
                self_stopped = True
                break

            cur_bad = src._parser.bad_crc_count
            if src.lost != last_lost or cur_bad != last_bad:
                events.append((time.time() - t_start,
                               src.lost - last_lost, cur_bad - last_bad))
                last_lost, last_bad = src.lost, cur_bad

        # 关键：STOP 是在**录音过程中**发的，这正是帧协议存在的理由
        if self_stopped:
            print(">>> 固件已自行判停（by=VAD），跳过发 STOP")
        else:
            print(">>> 录音结束，发送 STOP")
            src.send_control("STOP")
        src.drain(1.5)          # 排空尾帧，顺便收 STOPPED 状态

        samples = rec.close()
        st = src.stats()
        status = parse_status(st["last_status"])
    finally:
        src.close()

    # ---- 判定 ----
    #
    # ⚠️ 基准时长用**固件自报的 dur_ms**，不是脚本计划的 seconds。
    #    D13 之后固件可能自己判停（S3 的板端 VAD），实际录音短于计划是**正常**的 ——
    #    用计划值会把"固件正常自停"误判成"没录满"。（实测踩过：录了 41.5s 被判失败）
    dur_ms = status.get("dur_ms")
    by = str(status.get("by", "?"))
    expect_samples = (dur_ms * RATE // 1000) if dur_ms else seconds * RATE

    deficit = status.get("deficit")
    tx_block = status.get("tx_block_ms")

    total_frames = st["frames"] + st["lost"]
    loss_rate = st["lost"] / total_frames if total_frames else 0.0

    checks = [
        # ⚠️ 判据从「丢帧 = 0」放宽为「丢失率 < 1%」，依据见
        # docs/stage2-results.md §7：丢失源于 WSL USB 透传环境的偶发
        # ~500ms 停顿（CP2102N 的 512 字节缓冲只有 15.8ms 容错窗口），
        # 而非代码缺陷 —— 同一份代码零丢帧的长跑已经证明代码正确。
        ("丢失率 < 1%", loss_rate < 0.01,
         f"{loss_rate * 100:.3f}%  (丢 {st['lost']}/{total_frames} 帧)"),
        ("I²S 无丢样本（deficit≈0）", deficit is not None and abs(deficit) < 1000,
         f"deficit={deficit}"),
        ("样本数符合固件自报时长（±1%）",
         abs(samples - expect_samples) <= expect_samples * 0.01,
         f"{samples}/{expect_samples}"),
        # CRC 错误不再是失败项：它与丢帧同源（停顿导致中途中字节），
        # 只作信息上报。若它显著高于丢帧数，才说明另有问题。
        ("CRC 错误不高于丢帧数", st["bad_crc"] <= st["lost"] + 5,
         f"bad_crc={st['bad_crc']}"),
        ("TX 阻塞轻微（<64ms）", tx_block is not None and tx_block < 64,
         f"tx_block_ms={tx_block}"),
        # PC 侧排空能力。持续增长 = 排空速度跟不上到达速度 → 迟早溢出丢字节。
        # 这一项是给长时验收用的：60 秒看不出来，600 秒能。
        ("串口缓冲无积压（<8KB）", st["max_in_waiting"] < 8192,
         f"max_in_waiting={st['max_in_waiting']}"),
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
    # ---- 丢失事件的时间分布（判定病因的关键）----
    if events:
        times = [e[0] for e in events]
        first, last = times[0], times[-1]
        n = len(events)
        print()
        print(f"  丢失事件分布：共 {n} 次，首次 @ {first:.1f}s，末次 @ {last:.1f}s")
        if n >= 3 and last > 0:
            # 把时间轴均分 5 段，看事件是否均匀
            buckets = [0] * 5
            for t in times:
                buckets[min(4, int(t / last * 5))] += 1
            print(f"    按时间五等分: {buckets}")
            if first < 5 and buckets[0] > n * 0.5:
                print("    → 集中在开头 = 启动残留（monitor 遗留/上一次会话的尾巴）")
            else:
                print("    → 分散分布 = 链路噪声，与运行时序相关")
            gaps = [round(times[i+1]-times[i], 1) for i in range(min(12, n-1))]
            print(f"    相邻事件间隔(秒): {gaps}")

    print(f"  帧数 {st['frames']}  时长 {samples / RATE:.1f}s")
    print(f"  停止来源: by={by}"
          + ("  ← 固件板端 VAD 自己判停（D13）" if by == "VAD"
             else "  ← PC 发的 STOP" if by == "CMD" else ""))
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
