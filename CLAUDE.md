# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这个仓库是什么

ESP32 裸机项目（ESP-IDF v5.5.5），目标板 **ESP32-S3-CAM (N16R8)**。
切换只需 `idf.py set-target esp32s3`，板级差异全部集中在 `voice_notes/main/board.h`。

> ⚠️ **2026-10-04：ESP32-PICO-V3-02 已移出验证范围**（`decisions.md` D14）。
> 它的代码路径**保留但不验证**，会静默腐化 —— **别假设它还能用**。

| 目录 | 是什么 | 状态 |
|---|---|---|
| `hello_world/` | 板子验证工程 + `test_board.py` | ✅ 已完成 |
| `voice_notes/` | **语音速记工具**（STT）：ICS-43434 采音 → PC 本地**整段**转写 → ollama 总结 | 🟢 阶段 1（采音）✅ · 阶段 2（串口流）✅ · 阶段 3（转写）未开始 |
| `xiaozhi-esp32/` | **小智 AI 语音助手**移植（对话式）。⚠️ 它是 **git submodule**，不是普通目录 | 🟢 阶段 1（编译烧录+联网+采音+唤醒）✅ · 阶段 2（摄像头）未开始 |

> ⚠️ **`xiaozhi-esp32/` 是 submodule**，有两个 remote：
>
> | remote | 指向 | 用途 |
> |---|---|---|
> | `origin` | `wuRDmemory/xiaozhi-esp32`（我们的 fork） | 推我们的改动，分支 **`s3cam-port`**。**fetch 走 HTTPS，push 走 SSH**（见下面坑 4） |
> | `upstream` | `78/xiaozhi-esp32`（官方） | 日后合并官方更新 |
>
> **四条 submodule 特有的坑：**
>
> 1. **新克隆本仓库后必须先 `git submodule update --init`**，否则 `xiaozhi-esp32/`
>    是个**空目录** —— 会以为代码丢了。
> 2. **在 submodule 里提交后，必须回父仓库再提交一次**（记录新的 commit 号），
>    而且**要先把 submodule 推到 origin** —— 否则父仓库指向一个远端不存在的 commit，
>    别人克隆下来 `submodule update` 会失败。
> 3. 改完 submodule 忘了推是这套方案最常见的翻车方式。
> 4. ⚠️ **`.gitmodules` 里必须用 HTTPS，不能用 `git@`**（2026-10-06 实测）：
>
>    | 协议 | 实测速度 | 12MB 仓库耗时 |
>    |---|---|---|
>    | `git@github.com:`（SSH） | **~25 KB/s** | **20 分钟以上** |
>    | `https://github.com/` | **~900 KB/s** | **< 1 分钟** |
>
>    **差 36 倍。** 不是网络问题 —— 同一时刻 HTTPS 直连 GitHub 有 743 KB/s、
>    阿里云镜像 5 MB/s，只有 git-over-SSH 这一条路病态地慢（疑与 WSL2
>    的 mirrored 网络模式有关）。**写 `git@` 会让每次 clone 卡 20 分钟，
>    而且看起来像"卡死"** —— 实际上它在慢慢传。
>
>    推送仍走 SSH（fork 是**公开**的，拉取不需要凭据；但推送需要鉴权，
>    而本机没有 HTTPS token）：
>    ```bash
>    git -C xiaozhi-esp32 remote set-url --push origin git@github.com:wuRDmemory/xiaozhi-esp32.git
>    ```
>    ⚠️ 这是**本机局部设置**，不随 `.gitmodules` 走。`git submodule sync` 会把它冲掉，
>    换机器后要重设一次。

### voice_notes 的固件结构（阶段 2 起）

```
main/
├── board.h          ★ 板级差异的唯一出处（引脚/时钟源/PSRAM/能力标志）
├── Kconfig.projbuild  板子选择菜单（默认按 IDF target 自动选）
├── app_main.c       主循环：非录音时收命令，录音时采音+发帧+轮询 STOP
├── session.c/h      IDLE/RECORDING 状态机 + deficit 统计
├── afe.h            ★ AFE 能力抽象（板端判停，D13）
├── afe_passthrough.c   PICO 实现：全空操作，永不上报事件
├── afe_espsr.c         S3 实现：⚠️ 当前是临时骨架，**尚未接入 ESP-SR**
├── i2s_mic.c/h      I²S 采音（阶段 1 已验证的配置，不要改）
├── audio_frame.c/h  帧编解码（纯 C，可在 PC 上用 gcc 单测）
├── transport.c/h    UART 帧收发 + TX 阻塞时长统计
└── diag.c/h         诊断命令（level/raw/rec/shift/chan/dc/vad）—— 不要删
```

### 双板支持（board.h）

> ⚠️ **2026-10-04（`decisions.md` D14）：PICO 已移出验证范围。**
> **S3-CAM 是唯一的目标板。** PICO 的代码路径**保留但不验证** ——
> 它从今往后是**"未验证"**状态，会随改动静默腐化。
> **别假设 PICO 还能用**，任何"PICO 上应该没问题"的说法都没有依据。
> 遇到 PICO 相关的旧结论，先想清楚它是否还成立。

**两块板子没有一个参数是相同的**，所以按板编译期选择，不"改数字通用"：

| | PICO-V3-02（经典） | S3-CAM |
|---|---|---|
| I²S 引脚 | 26 / 25 / 22 | **14 / 21 / 47** |
| 时钟源 | `I2S_CLK_SRC_APLL` | `I2S_CLK_SRC_DEFAULT`（S3 无 APLL） |
| Flash / PSRAM | 8MB / 2MB quad | **16MB / 8MB octal** |
| console | UART0 @921600 | **USB-Serial-JTAG** |
| 设备节点 | `/dev/ttyUSB0` | **`/dev/ttyACM0`** |

**切换只需 `idf.py set-target <芯片>`** —— 板子会自动选中（Kconfig 按 target 给默认值），
`board.h` 按 `CONFIG_BOARD_*` 给参数。**不需要手工改任何文件。**

⚠️ **GPIO22/25 在 S3 上物理不存在**（S3 只有 0–21、26–48），且给 S3 选的引脚
在经典 ESP32 上会撞 SPI Flash —— 这就是为什么必须编译期选板，而不是共用一套引脚。

⚠️ **CAM 板的引脚极其紧张**：摄像头（AI-Thinker 映射）吃掉 `4–13, 15–18`，
加上 Flash/PSRAM(`26–37`)、USB(`19/20`)、UART0(`43/44`)、strapping(`0/3/45/46`)、
SD(`38–40`)、JTAG(`39–42`)、LED(`2/48`)，**干净的引脚只剩 14 / 21 / 47 三个**。
而且**摄像头引脚不能挪用** —— VSYNC/HREF 是摄像头**输出**，抢用会导致两个器件
同时驱动同一根线。

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

> ⚠️ **判停要做在板端（D13），别在 PC 侧继续加深逻辑。**
> 2026-10-04 用户要求参考 xiaozhi：「**完全参考 xiaozhiAI 就行，他的反应挺快的**」。
> xiaozhi 的"快"来自 **板端 AFE VAD 判停**，而现状是 PC 侧 RMS **静音 5 秒**才停 ——
> 用户说完要干等 5 秒，是整条链路上**最大的一段纯浪费**。
>
> ⚠️ **只抄判停，不抄识别。** xiaozhi 另一半的"快"来自云端**流式 ASR**，
> 那条会**丢掉标点**，与 D12 直接冲突。见 `decisions.md` D13 / D13.1。
>
> ⚠️ **AFE/VAD 只有 S3 有**（经典 ESP32 没有）→ PICO 上继续走 PC 侧 RMS，
> 由 `board.h` 的 **`BOARD_HAS_ESPSR`** 决定走哪条实现。
>
> ⚠️ **边界（重要）**：板端**只判断"有没有人在说 / 说完了没"，不识别"说了什么"**。
> 这是 `prd.md` §1.3「板端语音识别」放宽后的**新边界** ——
> **越过它就是在 MCU 上做 ASR，那是 D3 明确排除的方向。**

### voice_notes 已验收的固件配置（阶段 1，**PICO + S3 双板实测通过**）

> S3 侧的复验见 `s3-bringup.md` §5.5 —— 除引脚与时钟源来自 `board.h` 外，
> **下面的配置在 S3 上原样成立**（含之前从未验证的「64 SCK/帧 在 I²S HW v2 上是否一致」）。

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
| `hello_world/docs/wakeword-research.md` | **唤醒/聆听结束调研**：xiaozhi 的做法。⚠️ **结论已因换 S3 反转**（原文说"不可用"是针对经典 ESP32），顶部有注记 |
| `hello_world/docs/s3-bringup.md` | **S3 首次上板记录**：实测参数、4 个移植坑、工作流发现 |
| `hello_world/docs/plans/` | 各阶段的实现计划 |
| `hello_world/docs/cloud-asr.md` | 云 ASR 申请指引（当前走本地，不用） |
| `hello_world/docs/modules/` | ICS-43434 数据手册（PDF + 可 grep 的 txt） |

**voice_notes 的关键约束（动手前必读 `prd.md` §6）：** ICS-43434 要求
**每 WS 帧正好 64 个 SCK**，所以 ESP32 侧必须用 `I2S_SLOT_MODE_STEREO` +
32-bit 槽 —— 直觉上的 `MONO` 会得到 32 SCK/帧，麦克风直接不工作。
另：`dma_frame_num <= 511`（`dma_buffer_size <= 4092` 字节）。

---

## 小智固件（`xiaozhi-esp32/`）—— 配网 / 激活 / 唤醒

> 移植记录与验收证据：`hello_world/docs/plans/2026-10-04-xiaozhi-s3cam-port.md`
> 板级配置与说明：`xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/README.md`

### ⚠️ 前提：这块板子**没屏也没喇叭**

上游默认有屏有喇叭。本板两样都没有，这**决定了两件事**：

| 缺什么 | 后果 | 处理 |
|---|---|---|
| 屏幕 | 状态、识别文字没处显示 | **不用管** —— `Display::SetChatMessage()` 基类实现本身就会 `ESP_LOGW` 打到串口，识别文字**自动进日志** |
| 喇叭 + 屏幕 | **验证码无处可去** | **必须打补丁** —— 见下面「服务器激活」 |

### ① 配网

**没有存 WiFi 凭据时，设备开机会自动进配网模式**，不需要按键：

```
WifiConfigurationAp: Access Point started with SSID Xiaozhi-C391
esp_netif_lwip: DHCP server started ... IP: 192.168.4.1
```

- 热点名 = **`Xiaozhi-` + softAP MAC 的后两字节**
  ⚠️ **陷阱**：是 **softAP MAC**（= STA MAC + 1），不是 STA MAC。
  本板 STA MAC 是 `b8:1f:3f:ab:c3:**90**`，但热点名是 **`Xiaozhi-C391`**（`…c3:**91**`）。
  按 STA MAC 猜会猜成 `C390`，**猜错**。（我踩过这条。）
- 手机连上该热点 → 浏览器开 **`http://192.168.4.1`** → 填家里 WiFi
- 连上热点后手机提示"无互联网连接"是**正常的**，别切走
- 设备需要能访问**外网**才能激活

**BOOT 键在配网里的作用**：只在**启动阶段**（`kDeviceStateStarting`）按下才进配网模式；
启动完成后按它就是切换对话状态（见下面「唤醒」）。**所以别指望用 BOOT 键重新配网。**

### ② 服务器激活（每台设备一次）

设备连上网后去连 `api.tenclass.net`（OTA 端点，**同时负责下发服务器地址**），
拿到 **6 位激活码** → 用户去 <https://xiaozhi.me> 注册 → 控制台添加设备 → 输入该码。

> ⚠️ **验证码必须靠我们的串口补丁才能拿到 —— 这不是洁癖，是必需品。**
>
> 上游 `Application::ShowActivationCode()` 只把码交给**屏幕显示**和**语音播报**两条路。
> `Alert()` 里虽然打了 `ESP_LOGW`，但打的是 `message` **提示语，不是 `code` 本身**。
> 本板无屏无喇叭 → 不补丁就**永远激活不了**。
>
> 补丁位置：`main/application.cc` 的 `ShowActivationCode()` **开头**一行 `ESP_LOGW`。
> **别当调试残留清掉。** 将来若配上屏幕或喇叭，才可以删。

⚠️ **激活码有有效期**：固件只轮询 `Activate()` 约 **10 次**（每次间隔 3~10 秒）。
错过了就**重启板子**拿新码 —— 新码会立刻再打出来。

### ③ 唤醒与对话

激活成功后 `State: activating -> idle`，AFE 挂上 WakeNet 开始听：

```
AfeAudioEngine: Model 0: wn9_nihaoxiaozhi_tts
AFE: AFE Pipeline: [input] -> |VAD(WebRTC)| -> |WakeNet(wn9_nihaoxiaozhi_tts,)| -> [output]
AudioCodec: Set input enable to true
```

- **唤醒词：`你好小智`**
- **嘴要离麦克风 20cm 以内** —— 本模块灵敏度低于规格（见上面 voice_notes 那节）
- 唤醒成功的判据（**每一环都有日志，没有黑盒**）：
  ```
  Application: Wake word detected: 你好小智     ← WakeNet 在板端判出
  StateMachine: State: idle -> connecting
  StateMachine: State: connecting -> listening
  Display: Role:user
  Display:      你好小智                        ← STT 结果，自动进串口
  ```
- **BOOT 键**（运行中按）= 手动触发一轮对话。判据：也会 `idle -> connecting`，
  但**没有** `Wake word detected` 那行 —— **这就是区分「按键」和「唤醒词」的依据**。
  （`GPIO0` 已实测确认。）

### ⚠️ 麦克风：ICS-43434 的 64 SCK/帧硬要求，**不能用上游 codec**

上游 `main/audio/codecs/no_audio_codec.cc` 把麦克风配成
`I2S_SLOT_MODE_MONO` + 32bit = **32 SCK/帧**。而 ICS-43434 要求**每帧正好 64 SCK**
（与 voice_notes 那条约束同源）—— 用上游配置**麦克风完全不出数据**，
不是音质差，是**根本没数据**。

所以本板自带 `ics43434_codec.{h,cc}`，与上游两处差异：

1. 麦克风改用 `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32BIT, STEREO)` = 64 SCK/帧
2. `Read()` 用 `>>16`（上游 `>>12` 是按 INMP441 调的音量增益）。
   24bit 数据在 32bit 字的 `[31:8]`，取左声道后 `>>16` 才对。

**✅ `>>16` 已实测可用**（一次唤醒成功），**不必**向 `>>12` 靠拢。
`ICS43434_SHIFT` 是独立宏，要调只改一处。

### 构建与烧录

```bash
oesp                                    # 必须先激活（硬依赖）
cd xiaozhi-esp32
idf.py set-target esp32s3
# 选板：把 sdkconfig 切到 CONFIG_BOARD_TYPE_ESP32_S3_CAM_ICS43434=y
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

⚠️ **IDF 版本是硬约束**：上游 **v2.4.0 起主线已迁到 ESP-IDF 6.0+**，
v2.5.0 明确**不支持 5.x**。本机是 **5.5.5**，所以基线锁在 **v2.3.0**（要求 5.4+，
2026-07-15，功能上并不缺东西 —— s3cam 板子和摄像头抽象都在）。
**要跟进上游就得另装 IDF 6.1**（两套环境可并存，voice_notes 继续用 5.5.5）。

⚠️ `CONFIG_BOARD_TYPE_*` 是 **choice**：切换板子时要**同时**把旧板子设成
`# CONFIG_..._OLD is not set`，只把新的设 `=y` 是不够的。

### 抓串口日志

```bash
oesp && python xiaozhi_monitor.py -t 120        # 复位并抓 120 秒
oesp && python xiaozhi_monitor.py --no-reset    # 不复位，只监听
```

脚本**先开端口再复位**（顺序反了会丢开头，而 panic 和激活码恰恰都在开头），
并能在芯片重启导致 CDC 瞬断时**自动重连**（配网后重启就会发生）。
抓到激活码会提前退出。

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

## 目标板：**ESP32-S3-CAM**（当前唯一目标）

| | |
|---|---|
| 芯片 | ESP32-S3 (LX7)，rev v0.2 |
| Flash / PSRAM | **16 MB / 8 MB 八线（octal）** |
| USB | **原生 USB-Serial-JTAG** → `/dev/ttyACM0` |
| 能力 | **支持 ESP-SR**（AFE / VAD）→ 板端判停可用 |

**切换**：`idf.py set-target esp32s3`（Kconfig 自动选 `BOARD_S3_CAM`）。
详见 `hello_world/docs/s3-bringup.md`。

### ⚠️ 模组手册的两条勘误（2026-10-06 查 ESP32-S3-WROOM-1 数据手册 v1.4）

> 出处：`hello_world/docs/modules/esp32-modula.pdf`
> （⚠️ 文件名是 **module** 的笔误。它是 **WROOM-1 模组**手册，**不是 CAM 板**手册。）
> 可 grep 的文本版：同目录 `esp32-modula.txt`（⚠️ **中文是乱码** —— CFF Type1 字体
> 编码问题，fontTools 也修不好；**引脚号和英文功能名是好的，读那些**）。

**① `IO35 / IO36 / IO37` 在 N16R8 上不可用**
手册脚注 b：集成 Octal SPI PSRAM 的 **ESP32-S3R8 / S3R16V** 会复用这三个脚。
比"26–37 全被占"更精确的边界。

**② ⚠️ `IO47 / IO48` 的电平取决于模组变体**
手册脚注 c：**ESP32-S3R16V**（16 MB PSRAM）模组里 VDD_SPI 被设为 1.8 V，
**IO47/IO48 随之也是 1.8 V**。

> 这对我们是个**差点踩到**的坑：**麦克风 DIN 正好在 GPIO47**。
> 本板是 **N16R8 = S3R8** → 脚注 c **不适用** → IO47 是正常 **3.3 V，当前接线安全**。
>
> ⚠️ **但若将来换成 N16R16V 的模组，GPIO47 必须挪走** —— 否则麦克风信号电平不匹配，
> 表现会是"能编译、能启动、麦克风就是没数据"，且极难联想到是模组变体问题。
>
> **推论（重要）：「干净引脚」的结论是绑定模组变体的。换模组要重新核对，不能照搬。**

### ❌ 摄像头引脚**不在**模组手册里，别再去那儿找

模组手册 `§5.2.1.4` 明确写：**「LCD 与 Camera 可以为任意 GPIO 通过 GPIO 交换矩阵配置」**。
管脚定义表（表 3）里每个脚列的都是 `RTC_GPIO / TOUCH / ADC / FSPI / SUBSPI / USB_D± /
MTCK-MTMS / U0RXD-U0TXD / CLK_OUTx / XTAL_32K`，**一个摄像头信号都没有**。

**原因**：ESP32-S3 的 LCD_CAM 走 GPIO 交换矩阵，**任意 GPIO 都能当摄像头脚** ——
所以模组手册按设计就**不会**分配摄像头引脚，那是**板厂（AI-Thinker）的设计决定**。

**要找摄像头引脚，得看 AI-Thinker 的板级原理图。** 模组手册只能告诉你外设规格：
**8~16 位 DVP、时钟 < 40 MHz、支持 RGB565 / YUV422 / YUV420 / YUV411**。

---

## ~~目标板：ESP32-PICO-V3-02~~（**已停止验证**，见 D14）

> ⚠️ **以下内容自 2026-10-04 起仅为历史记录。** PICO 不再验证，
> 其代码路径保留但会静默腐化。**新的排查/改动不要依此行事。**

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

> ⚠️ **下面写的是 PICO（CP2102N）的路径。S3 走原生 USB，有两处不同**：
> ① 设备节点是 `/dev/ttyACM0` 不是 `/dev/ttyUSB0`；
> ② **不需要 `modprobe`** —— `cdc_acm` 内建于 WSL 内核（`CONFIG_USB_ACM=y`）。
> 闸门 ①②（usbipd bind/attach）和 ④（chmod）**两者相同**。
> 串口要 auto-detect：`serial_src.default_port()` 已实现（S3 优先）。

> ⚠️ **usbipd 的「假 Attached」陷阱**（2026-10-04 踩到，浪费过时间）：
> **WSL 重启后，`usbipd list` 会继续显示 `Attached`，但 WSL 内核里根本没有这个设备**
> —— `/sys/bus/usb/devices/` 里只有根 hub，`dmesg` 里也没有接入事件。
> 此时 `attach` 会报 `error: Device with busid 'X-Y' is already attached to a client`，
> **听起来像"已经好了"，其实完全不通**。
>
> **解法**：先 `detach` 再 `attach`。
> ```bash
> "/mnt/c/Program Files/usbipd-win/usbipd.exe" detach --busid 4-3   # 无输出即成功
> "/mnt/c/Program Files/usbipd-win/usbipd.exe" attach --wsl --busid 4-3
> ```
> `detach`/`attach` 都**能从 WSL 里直接调 `.exe`，不需要管理员权限**。
>
> **先看这两个判据再决定要不要重绑**：`ls /sys/bus/usb/devices/`（应有 `1-1` 之类，
> 不是只有 `usb1`/`usb2`）、`ls /dev/ttyACM*`。**别只看 `usbipd list` 的 STATE 列。**

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
idf.py -p /dev/ttyACM0 flash monitor          # 烧录 + 看串口 (Ctrl+] 退出)

oesp && python ../test_board.py               # 端到端验证：硬复位+抓日志+发数据验回显
```

> ⚠️ **设备节点是 `/dev/ttyACM0`，不是 `/dev/ttyUSB0`** —— 这是 D14 换板后的遗留更正：
> S3 走**原生 USB**（`303a:1001`），PICO 才走 CP2102N（`/dev/ttyUSB0`）。
> 而 PICO 已停止验证，**所以本仓库现在只该出现 `/dev/ttyACM0`**。
> 见到 `/dev/ttyUSB0` 就是陈旧内容，照做会打不开端口。
>
> ⚠️ 另：`test_board.py` 里若还写死着 `/dev/ttyUSB0`，同样要按这条改
> （voice_notes 侧已修过同类问题，见 commit `8ab00f7`）。

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
