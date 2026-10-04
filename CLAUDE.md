# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这个仓库是什么

ESP32 裸机项目（ESP-IDF v5.5.5），跑在一块 **ESP32-PICO-V3-02** 开发板上。

| 目录 | 是什么 | 状态 |
|---|---|---|
| `hello_world/` | 板子验证工程 + `test_board.py` | ✅ 已完成 |
| `voice_notes/` | **语音速记工具**（STT）：ICS-43434 采音 → PC 本地**整段**转写 → ollama 总结 | 🟢 阶段 1（采音）✅ · 阶段 2（串口流）✅ · 阶段 3（转写）未开始 |

### voice_notes 的固件结构（阶段 2 起）

```
main/
├── board.h          ★ 板级差异的唯一出处（引脚/时钟源/PSRAM/能力标志）
├── Kconfig.projbuild  板子选择菜单（默认按 IDF target 自动选）
├── app_main.c       主循环：非录音时收命令，录音时采音+发帧+轮询 STOP
├── session.c/h      IDLE/RECORDING 状态机 + deficit 统计
├── i2s_mic.c/h      I²S 采音（阶段 1 已验证的配置，不要改）
├── audio_frame.c/h  帧编解码（纯 C，可在 PC 上用 gcc 单测）
├── transport.c/h    UART 帧收发 + TX 阻塞时长统计
└── diag.c/h         诊断命令（level/raw/rec/shift/chan/dc）—— 不要删
```

### 双板支持（board.h）

**两块板子没有一个参数是相同的**，所以按板编译期选择，不"改数字通用"：

| | PICO-V3-02（经典） | S3-DevKitC-1 |
|---|---|---|
| I²S 引脚 | 26 / 25 / 22 | **5 / 6 / 7** |
| 时钟源 | `I2S_CLK_SRC_APLL` | `I2S_CLK_SRC_DEFAULT`（S3 无 APLL） |
| Flash / PSRAM | 8MB / 2MB quad | **16MB / 8MB octal** |
| console | UART0 @921600 | **USB-Serial-JTAG** |
| 设备节点 | `/dev/ttyUSB0` | **`/dev/ttyACM0`** |

**切换只需 `idf.py set-target <芯片>`** —— 板子会自动选中（Kconfig 按 target 给默认值），
`board.h` 按 `CONFIG_BOARD_*` 给参数。**不需要手工改任何文件。**

⚠️ **GPIO22/25 在 S3 上物理不存在**（S3 只有 0–21、26–48），且给 S3 选的引脚
在经典 ESP32 上会撞 SPI Flash —— 这就是为什么必须编译期选板，而不是共用一套引脚。

完整记录见 `hello_world/docs/s3-bringup.md`。

### ⚠️ 已知环境限制：WSL USB 透传会偶发丢音频

**每 10 分钟约丢 0.5 秒**（31~44 帧，10 分钟跑两次都是这个量级）。
**这不是代码问题** —— 同一份代码 5 分钟跑出零丢帧，**若为代码缺陷，丢失应与
时长成正比而不是 0**。完整排查见 `stage2-results.md` §7。

机制：CP2102N（在此链路上）只有 **512 字节缓冲 = 15.8ms 容错窗口**，
一次约 500ms 的 USB 停顿就会溢出。usbip 以引入延迟著称，这是它的已知代价。

**动手排查前先排除这四件事**（都已验证清白，别重复查）：
固件侧 `deficit`/`tx_block_ms`、`uart_write_bytes` 的返回语义、
PC 侧排空能力（看 `max_in_waiting`）、以及帧协议本身。

**验收标准因此定为「丢失率 < 1%」而非「丢帧 = 0」。** 详见 `stage2-results.md` §7.5。

**两条容易搞错的认知**（详见 `stage2-results.md`）：

1. **`deficit` 比 `seq` 更本质。** `seq` 由固件生成，序号连续只说明"发出的帧
   都到了"，**不能说明 I²S 没漏采样本** —— 漏采的样本根本不会变成帧。
2. **`lost>0` 但 `bad_crc=0` 是自相矛盾的组合**，出现它先怀疑自己的度量而不是
   链路：真丢字节必然拼出半帧、CRC 校验失败。（这个组合我们真踩过，
   查了半天发现是 drain 的统计顺序问题，数据压根没丢。）

> ⚠️ **别把流式转写做回来。** 2026-09-27 用户明确取消了流式（`prd.md` D12）：
> 录音期间只显示「聆听中 + 计时 + 电平条」，**不转写、不渲染文字**；
> 停止后对整段录音一次性转写。
>
> 取消流式**不是为了省事**，而是解锁了 `ct-punc` 标点恢复 —— 带标点的文本
> 对 LLM 总结质量影响很大。**所以 PRD 里凡看到"流式"字样，都是 v1.0 的遗留
> 或对它的说明，不是当前设计。** 动手前先看 `prd.md` 头部的变更记录。

### voice_notes 已验收的固件配置（阶段 1，实测通过）

`mic_test.c` 里的 I²S 配置**已验证可用，改之前先看 `hardware.md` §7**。
最省事的做法是照抄，不要重新推导：

- 16 kHz · `I2S_CLK_SRC_APLL` · **`I2S_SLOT_MODE_STEREO` + 32bit**（= 64 SCK/帧）
- `dma_frame_num = 256`（上限 511）· GPIO BCLK=26 / WS=25 / DIN=22
- 24bit 数据在 32bit 字的 `[31:8]`，`>>16` 得 16bit

**这块麦克风模块的两个实测特性**（原厂规格里没有，容易被误判成故障）：

1. **有效分辨率约 19 位**：原始 32bit 字的最低 13 位恒为 0。国产克隆模块的正常特征，
   **不是 bug**，别去"修"。
2. **灵敏度低于规格**：安静时底噪 −60 dBFS，拍手瞬态才 −33 dBFS。
   → **录音时嘴要离麦克风 20cm 以内**，远距离拾不到。
   → 判断麦克风好坏用「拍手能否让电平跳 20 dB 以上」，别用绝对电平。

`mic_test.c` 的 `raw` 命令是判定位对齐的工具，`level` 是验证麦克风是否响应的工具 ——
遇到"能出数据但不对"的情况先跑这两个。

### 📁 需求文档在 `hello_world/docs/`（**不在仓库根目录**）

反直觉但属实 —— 用户指定的位置。**找需求文档别去根目录找。**

| 文件 | 内容 |
|---|---|
| `hello_world/docs/prd.md` | 产品需求文档：架构、I²S 配置推导、风险登记册、用户待办 |
| `hello_world/docs/decisions.md` | 14 条决策及依据 —— **改需求前先看这个**，避免重复讨论 |
| `hello_world/docs/hardware.md` | 接线、零件、逐级上电验证、**阶段 1 验收记录（§7）** |
| `hello_world/docs/stage2-results.md` | **阶段 2 验收记录**：实测数据、发现的两个 bug、关键认知 |
| `hello_world/docs/wakeword-research.md` | **唤醒/聆听结束调研**：xiaozhi 的做法、为什么我们芯片用不了、唯一值得借鉴的点 |
| `hello_world/docs/s3-bringup.md` | **S3 首次上板记录**：实测参数、4 个移植坑、工作流发现 |
| `hello_world/docs/plans/` | 各阶段的实现计划 |
| `hello_world/docs/cloud-asr.md` | 云 ASR 申请指引（当前走本地，不用） |
| `hello_world/docs/modules/` | ICS-43434 数据手册（PDF + 可 grep 的 txt） |

**voice_notes 的关键约束（动手前必读 `prd.md` §6）：** ICS-43434 要求
**每 WS 帧正好 64 个 SCK**，所以 ESP32 侧必须用 `I2S_SLOT_MODE_STEREO` +
32-bit 槽 —— 直觉上的 `MONO` 会得到 32 SCK/帧，麦克风直接不工作。
另：`dma_frame_num <= 511`（`dma_buffer_size <= 4092` 字节）。

---

## 第一件事：激活环境

**任何 `idf.py` 命令前必须先 `oesp`。** 这不是习惯问题，是硬依赖。

```bash
oesp     # 进
doesp    # 出
```

`oesp` 定义在 `~/.zshrc`，做三件事：`conda deactivate` → `unset IDF_PYTHON_ENV_PATH` → `source ~/esp/esp-idf/export.sh`。

**为什么必须**：IDF v5.5 要求 Python ≥3.9，系统自带的是 3.8；而这台机器的默认 `python3` 是 conda base 的 3.12，会跟 IDF 的 `install.sh` 抢解释器、建错 venv（espressif/esp-idf#12071）。方案是用独立 conda 环境 `esp32`(py3.11) 提供解释器，IDF 再基于它建自己的 venv。

### 布局

| 东西 | 位置 |
|---|---|
| ESP-IDF v5.5.5 | `~/esp/esp-idf` |
| 工具链 | `~/.espressif`（约 4GB） |
| IDF 的 venv | `~/.espressif/python_env/idf5.5_py3.11_env` |
| conda 环境 | `esp32` (Python 3.11) |
| 已装 target | `esp32` / `esp32s3` / `esp32c3` / `esp32c6` |

`xtensa-esp-elf` 是**统一工具链**，覆盖经典 esp32 + s2 + s3 —— 装了 s3 就顺带有 esp32，不必重复下载。

---

## 目标板：ESP32-PICO-V3-02（**别信型号自报**）

```
$ esptool.py --port /dev/ttyUSB0 flash_id
Chip is ESP32-PICO-V3-02 (revision v3.1)
Features: ..., Embedded Flash, Embedded PSRAM, ...
Detected flash size: 8MB
```

经典 ESP32（Xtensa LX6 双核）—— **不是** S3/C3/C6。8MB Flash、2MB PSRAM、CP2102N。

⚠️ **用户可能会说这块是 "ESP32-PICO-KIT V4"。** 但官方文档说 V4 用的是 PICO-D4（4MB、无 PSRAM、芯片 v1.0/v1.1），**与实测三条全不符** —— 国内大量第三方板叫 PICO-KIT 却装 V3-02 模块。
→ **板子身份一律以 `esptool flash_id` 为准**，它直接读 eFuse。丝印和型号自报都不可信。

---

## USB 链路：4 道闸门，缺一不通

这是本项目最耗时间的地方。设备要从 Windows 一路走到 WSL 的 `/dev/ttyUSB0`：

```
Windows USB 总线
  ↓ ①  usbipd bind --force          (Windows 管理员, 一次性)
  ↓ ②  usbipd attach --wsl          (Windows, 每次 WSL 重启/插拔后)
WSL 内核
  ↓ ③  sudo modprobe cp210x         (每个 WSL 实例一次)
  ↓ ④  sudo chmod 666 /dev/ttyUSB0  (每次 attach)
/dev/ttyUSB0
```

### 每道闸门存在的原因

**① `--force` 是必需的** —— 这台机器装了 UsbDk 过滤驱动，和 usbipd 抢同一层，不加会被拒。
代价（usbipd 官方原话）：`"Force binding; the host cannot use the device"` —— **Windows 永久用不了这块设备**，`detach` 也不还，只有 `unbind` 才能还。

> → **所以：用户说"Windows 看不到 COM 口"是预期行为，不是故障，不要去"修"。**
> → 只有用户明确要还给 Windows 时，才跑 `usbipd unbind --busid 4-4`。

**② `attach` 是内存态** —— WSL 重启必丢。**BUSID 是物理端口地址，换 USB 插口就变**，必须 `usbipd list` 重查。

**③④ 都源于：这台 WSL 的 PID 1 是 `init` 而不是 systemd，没有 udev daemon。**
- ③：Linux 正常靠 udev 读设备 modalias 自动 modprobe，这里得手动。**模块加载后常驻，每个 WSL 实例只需一次。**
- ④：devtmpfs 建节点用默认权限 `root:root 0600`。udev 在的话会按 Ubuntu 自带规则归入 `dialout` 组（用户在该组里，自动可用）。**每次 attach 都要重来**，节点重建权限就复位。

### 诊断命令（定位卡在哪一层）

```bash
"/mnt/c/Program Files/usbipd-win/usbipd.exe" list   # ①② 的状态
lsmod | grep cp210x                                  # ③
ls -la /dev/ttyUSB0                                  # ④
dmesg | tail -12                                     # 内核侧的真相
```

Windows 侧设备状态（判断驱动是否被 usbipd 接管）：
```bash
powershell.exe -NoProfile -Command "Get-PnpDevice | Where-Object { \$_.InstanceId -like '*10C4*' } | Format-List FriendlyName, Status, Problem"
```
绑定时会显示 `FriendlyName: USBIP Shared Device` —— 就是它把 CP210x 的 COM 驱动挤掉了。

### 陷阱

- **同一时刻只有一个程序能开 `/dev/ttyUSB0`。** 开着 `idf.py monitor` 时跑 `test_board.py` 会报 `multiple access on port` —— **那不是 bug**，先 `Ctrl+]` 退出监视器。
- `usbipd list` 的 `Persisted:` 段目前为空，**`bind` 能否扛过 Windows 重启未经验证**。重启后先看 STATE 列：显示 `Not shared` 才需要重新 bind。

---

## 构建 / 烧录 / 验证

```bash
cd hello_world
idf.py build                                  # 编译
idf.py -p /dev/ttyUSB0 flash monitor          # 烧录 + 看串口 (Ctrl+] 退出)

oesp && python ../test_board.py               # 端到端验证：硬复位+抓日志+发数据验回显
```

`test_board.py` 用 DTR/RTS 硬复位板子（DTR→GPIO0 启动模式，RTS→EN 复位），然后真的发数据并校验回显，不是只看有没有输出。

---

## ⚠️ sdkconfig 的静默失效坑（实测踩过）

**有些 `CONFIG_*` 写进 `sdkconfig.defaults` 会被 Kconfig 静默丢弃** —— 编译通过、
不报错、不警告，但值根本没生效。判据是 Kconfig 里那个符号**有没有 `prompt`**：
若 prompt 带条件（如 `prompt "..." if OTHER_SYMBOL`），条件不满足时该符号
**不可由用户赋值**，你写的值被忽略、回落默认值。

**已踩过的实例**：`CONFIG_ESP_CONSOLE_UART_BAUDRATE` 的 prompt 是
`if ESP_CONSOLE_UART_CUSTOM`（`components/esp_system/Kconfig:413`）。
只写波特率、不选 CUSTOM → **静默回落 115200**。

```kconfig
# ✅ 正确：必须连 CUSTOM 一起选，波特率才生效
CONFIG_ESP_CONSOLE_UART_CUSTOM=y
CONFIG_ESP_CONSOLE_UART_CUSTOM_NUM_0=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600
```

同族还有一批**无 prompt 的派生符号**（`ESP_CONSOLE_UART_NUM`、
`ESP_CONSOLE_UART` 等），它们的值由别的符号推导，直接赋值无效。

> **验证方法**：改完 `.defaults` 后**必须 grep 生成的 `sdkconfig` 复核**，
> 不能只看编译过没过。这条对所有 `CONFIG_*` 都适用。

---

## 改配置的正确姿势

改 `hello_world/sdkconfig.defaults`，**不要**改 `sdkconfig`。

**关键坑**：`sdkconfig` 里**已存在**的旧值**不会**被 `sdkconfig.defaults` 覆盖。改完 `.defaults` 必须重新生成：

```bash
rm -rf build sdkconfig
idf.py set-target esp32
grep -E "CONFIG_ESPTOOLPY_FLASHSIZE=|CONFIG_SPIRAM=" sdkconfig   # 复核
```

不这么做，你会以为改了却根本没生效。

---

## 这台机器的其他地雷

- **绝不跑 `apt upgrade` / `apt full-upgrade` / `dist-upgrade`。** 系统的 `libc6` 被手动 dpkg 装成了 **2.35**（jammy 的版本号，`apt-cache policy` 显示无任何仓库来源），而 tuna focal 源的候选版本是 **2.31** —— 升级有把它降级回去的风险，会同时打断 ROS Noetic 和整个 WSL 环境。只用 `apt install <包名>` 精确安装。
- PATH 上挂着 **ROS Noetic** 和 **10 个 conda 环境**（torch / cosyvoice / chatTTS / …）。不要碰它们，也不要假设 `python3` 是系统 Python。
- `/etc/wsl.conf` 里的 `[boot] systemd=true` 是注释掉的。启用它能一并解决 ③④（udev 会活过来），但需要 `wsl --shutdown`，且会影响 docker/ROS —— **改动前先问用户**。

---

## 代码约定

- 注释用中文。
- **别用 `LINE_MAX` 当宏名** —— 它是 POSIX `<limits.h>` 的系统宏，重定义会触发警告。已有先例：本项目改用了 `CMD_LINE_MAX`。
- 板子**没有用户可控 LED**（只有一颗 5V 电源指示灯），**不要写 blink 示例**。
- 串口读必须切阻塞模式：默认 console 是**非阻塞**的，直接 `fgetc` 会立刻返回 EOF（表现为"敲键盘没反应"）。需要 `uart_driver_install` + `uart_vfs_dev_use_driver`。参考 `main/hello_world_main.c` 的 `console_enable_blocking_read()`，那段是照 IDF 官方例子写的，不要自己发明。

  > ⚠️ **这条已经违反过一次，代价是用户完全没法用。** 当时在 `voice_notes/main/mic_test.c`
  > 里写了段注释给自己开脱（"本固件命令少，非阻塞够用"），结果 console 非阻塞 +
  > stdin 不按行缓冲 → **敲进去的每个字符被切碎、逐个当指令**，同时提示符疯狂刷屏
  > （`mic> mic> mic> mic> ...`）。
  >
  > **"我有理由所以可以不做"是本项目最危险的一种想法。** 见到这类清单项，照做。

### 测试硬件交互时的陷阱：瞬时输入会掩盖缓冲 bug

上面那个 bug **我自己的自动化测试全都"通过"了**，因为脚本一次性 `write(b"cfg\n")` ——
4 个字节同时到达，非阻塞的 `fgets` 正好能凑齐一整行。**但人手敲键慢，每个字符单独
到达就被切碎。** 症状只在人手里出现。

→ **凡是验证"人机输入"相关的改动，测试必须模拟人的时序**：

```python
def send_slow(ser, text, per_char=0.08):   # 逐字符 + 间隔
    for ch in text:
        ser.write(ch.encode()); time.sleep(per_char)
    ser.write(b"\n")
```

推而广之：**测试通过 ≠ 用户能用。** 问自己"我的测试和真人操作差在哪"。

### 串口被占用是常态，不是故障

`Could not open /dev/ttyUSB0, the port is busy` 几乎总是**用户开着 `idf.py monitor`**。
先查是谁占的，别去怀疑硬件：

```bash
lsof /dev/ttyUSB0          # 或扫 /proc/*/fd
ps -eo pid,etime,args | grep -E "esp_idf_monitor|idf.py .*monitor"
```
