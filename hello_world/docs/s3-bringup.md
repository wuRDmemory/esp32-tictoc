# ESP32-S3 首次上板记录（2026-10-04）

> ### ⚠️ 更正：板子是 **ESP32-S3-CAM**，不是 ESP32-S3-DevKitC-1
>
> 本文写作时按 DevKitC-1 假设，**引脚部分（§2 ①）基于错误的板型**。
> 用户后来更正了型号。**芯片层面的发现全部仍然有效**（PSRAM octal、
> 无 APLL、原生 USB、16MB flash 等都与板型无关），只有引脚选择作废：
>
> | | 当时按 DevKitC-1 选的 | 实际可用的 |
> |---|---|---|
> | I²S 引脚 | 5 / 6 / 7 | **14 / 21 / 47** |
> | 为什么作废 | —— | **5/6/7 全是摄像头引脚**（SIOC/VSYNC/HREF） |
>
> 教训：**先问清板型再查引脚**。CAM 板与 DevKitC 的差别不是细节 ——
> 摄像头吃掉十几个 GPIO，可用的干净引脚只剩三个。

**背景**：为 voice_notes 引入第二块板子 **ESP32-S3-DevKitC-1 (N16R8)**。
经典 ESP32（PICO-V3-02）继续用于调试，S3 是最终目标（它支持 ESP-SR 的
AFE/VAD/WakeNet，经典 ESP32 不支持 —— 见 `wakeword-research.md`）。

**做法**：**先把 S3 的编译和上板走通，再动架构。** 事实证明这个顺序是对的 ——
下面 4 个问题如果留到"换板子那天"才遇到，现场表现全都像硬件故障。

---

## 1. 硬件实测参数（权威，不要重新推导）

```
$ esptool --chip esp32s3 -p /dev/ttyACM0 chip_id
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
USB mode: USB-Serial/JTAG
MAC: b8:1f:3f:ab:c3:90

$ esptool --chip esp32s3 -p /dev/ttyACM0 flash_id
Detected flash size: 16MB
```

| 项 | 值 | 对比 PICO-V3-02 |
|---|---|---|
| 芯片 | ESP32-S3 (LX7)，rev v0.2 | 经典 ESP32 (LX6)，v3.1 |
| Flash | **16 MB** | 8 MB |
| PSRAM | **8 MB，八线（octal）** | 2 MB，四线 |
| 晶振 | 40 MHz | 40 MHz |
| USB | **原生 USB（USB-Serial/JTAG）** | CP2102N 桥接 |
| 模组 | ESP32-S3-WROOM-1 **N16R8** | PICO-V3-02 |

---

## 2. 挖出的 5 个移植问题

**全都是"能编译过但跑不起来"或"根本编译不过"，且症状都具有误导性。**

其中 **⑤ 最隐蔽** —— 前四个要么编译报错、要么启动就崩，**只有它编译通过、
启动日志完全正常，只是不工作**。

### ① GPIO22 / GPIO25 在 S3 上【物理不存在】

```
error: 'GPIO_NUM_25' undeclared ; did you mean 'GPIO_NUM_45'?
error: 'GPIO_NUM_22' undeclared ; did you mean 'GPIO_NUM_42'?
```

| 芯片 | GPIO 范围 |
|---|---|
| 经典 ESP32 | 0–39 |
| **ESP32-S3** | **0–21, 26–48** |

**S3 没有 22、23、25。** 我们原来的 `WS=25 / DIN=22` **两个都不存在**。

> ⚠️ **这不是"改个数字"能解决的** —— 给 S3 选的引脚在经典 ESP32 上会撞
> SPI Flash（GPIO6–11），反之亦然。**必须按板编译期选择**，这正是 `board.h` 存在的理由。

**S3 引脚选取（以下逐条规避）**：

| 引脚 | 为什么不能用 |
|---|---|
| 22 / 23 / 25 | **不存在** |
| 26 – 37 | 八线 PSRAM/Flash 占用（`SPIRAM_CS_IO` 默认值就是 26） |
| 0 / 3 / 45 / 46 | strapping |
| 19 / 20 | 原生 USB（D−/D+） |
| 43 / 44 | UART0 |
| 38（旧版 48） | 板载 RGB LED |

→ **选 5 / 6 / 7**，都在安全区 1–21 内。

### ② `I2S_CLK_SRC_APLL` 在 S3 上不存在

```
error: 'I2S_CLK_SRC_APLL' undeclared ; did you mean 'I2S_CLK_SRC_XTAL'?
```

```c
// 经典 ESP32 (soc/esp32/clk_tree_defs.h)
I2S_CLK_SRC_APLL        = SOC_MOD_CLK_APLL        ← 有

// ESP32-S3 (soc/esp32s3/clk_tree_defs.h)
SOC_I2S_CLKS = {PLL_F240M, PLL_F160M, XTAL, EXTERNAL}
                                                   ← 没有 APLL
```

→ S3 用 `I2S_CLK_SRC_DEFAULT`（= PLL_F160M）。
精度影响：160 MHz ÷ 1.024 MHz = 156.25，靠小数分频，实测可用。

### ③ PSRAM 模式错了会【反复重启】，而且错误信息极有误导性

```
E quad_psram: PSRAM chip is not connected, or wrong PSRAM line mode
E cpu_start: Failed to init external RAM!
abort() was called at PC 0x42001d87 on core 0
Rebooting...
（然后无限循环）
```

**读起来像"板子坏了 / PSRAM 虚焊"，实际只是 Kconfig 默认值不对**：
Kconfig 的 `SPIRAM_MODE` 默认是 **QUAD**，而 N16R8 是 **OCTAL**。

```kconfig
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y            # ← 关键，默认是 QUAD
CONFIG_SPIRAM_TYPE_ESPPSRAM64=y     # 64 Mbit = 8MB
CONFIG_SPIRAM_SPEED_80M=y
```

> **教训**：看到 `Failed to init external RAM` 先查配置，别急着怀疑硬件。

### ④ Flash 容量假设错了（8MB → 实测 16MB）

写小了会让分区表只覆盖一半。已修正为 16MB。

### ⑤ ⚠️ 传输通路与 console 不是同一个外设（**最隐蔽**）

**症状**：启动日志完全正常，但**敲命令没反应、也发不出音频**。
很容易误判成接线问题或板子故障。

**根因**：`transport.c` 写死了 `UART_NUM_0`，而 S3 的 console 是 USB-Serial-JTAG：

| | PICO | **S3** |
|---|---|---|
| `printf` | UART0 | **USB-Serial-JTAG** ✓（所以日志能看到，**掩盖了问题**） |
| `transport_send` | UART0 | **UART0**（GPIO43/44，没接任何东西）✗ |
| `transport_poll` | UART0 | **UART0** ✗ |

**两者是不同外设，数据各走各的，永远碰不上。**

**修复**：transport 加通路抽象，由 `board.h` 的 `BOARD_TRANSPORT_USB` 选择。
顺带让 `diag` 的 `wav` 命令也用 `transport_write_raw()`（它原来也直接写 UART）。

> **教训**：抽象要抽象**完整的通路**，不只是上层协议。
> 我当时只抽象了**帧格式**，没抽象**物理链路** —— 换板时才漏出来。
> 这类 bug 的共同特征是：**编译全过、日志正常、就是不工作**，最难定位。

---

## 3. 工作流上的发现（原生 USB 的实际收益）

| 项 | CP2102N（PICO） | **原生 USB（S3）** |
|---|---|---|
| 内核驱动 | 模块 `=m` → 需 `modprobe` | **内建 `USB_ACM=y` → 不需要** |
| 设备节点 | `/dev/ttyUSB0` | `/dev/ttyACM0` |
| 烧录速率（实测） | ~643 kbit/s | **~1337 kbit/s（快一倍）** |
| **可能顺带解决丢帧** | 512 字节缓冲 = 15.8ms 容错窗口 | **完全绕开**（见 `stage2-results.md` §7） |

### `bind` 只需一次（前提是 console 配置对）

- 出厂 demo 固件占用 USB-OTG → 枚举为 `303a:4001`
- 进入下载模式后是 ROM 的 USB-Serial-JTAG → `303a:1001`
- **PID 变了 = Windows 认为是新设备 = 要重新 `bind --force`**

→ **把 console 配成 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` 后，下载模式与运行固件
都是 `1001`，PID 不再变化，就不用反复重绑。** 这是选它的重要理由之一。

### 复位行为不同

| 复位方式 | USB 是否重新枚举 |
|---|---|
| esptool 的软复位 | ❌ 不变 → 不用重新 attach / chmod |
| **物理按 RST** | ✅ **会重新枚举** → 需重新 attach + chmod |

### ⚠️ WSL 无 udev → 每次节点重建都要重新 chmod

`/dev/ttyACM0` 每次重建都是 `root:root 0600`。物理复位后要重新
`sudo chmod 666 /dev/ttyACM0`。**这是已知摩擦，尚未解决。**

---

## 4. 一个会误导排查的坑：BOOT 键 = 卡在下载模式

**症状**：串口反复输出

```
rst:0x15 (USB_UART_CHIP_RESET),boot:0x0 (DOWNLOAD(USB/UART0))
waiting for download
```

**看起来像**：烧录失败 / 固件不启动 / 芯片坏了。

**实际是**：**BOOT 键被按住**。`boot:0x0` 是 **strapping 引脚的采样结果** ——
GPIO0 低电平 = 每次复位都进下载模式。**软复位（包括 monitor 的 `reset` 命令）
改变不了它**，因为 strapping 是硬件采样的。

→ **松开 BOOT 再按 RST 即可。** 排查时先确认这一点，别去怀疑固件。

---

## 5. 交付物

| 文件 | 作用 |
|---|---|
| `main/board.h` | **板级差异的唯一出处**：引脚、时钟源、PSRAM、能力标志 |
| `main/Kconfig.projbuild` | 板子选择菜单，**默认值按 IDF target 自动选** |
| `sdkconfig.defaults` | 通用配置 |
| `sdkconfig.defaults.esp32` | 经典 ESP32 专属 |
| `sdkconfig.defaults.esp32s3` | S3 专属（含上面 4 个问题的修正） |

**验证结果**：

```bash
idf.py set-target esp32    && idf.py build   # ✅ 自动选 PICO_V3_02
idf.py set-target esp32s3  && idf.py build   # ✅ 自动选 S3_DEVKITC_1
```

**同一份源码，零手工修改。** S3 上板实测：PSRAM 8MB 就绪、I²S 初始化成功、
console 走原生 USB 正常。

---

## 5.5 S3 上麦克风的实测结果（2026-10-04 通过）

| 判据 | 结果 | 对照 PICO |
|---|---|---|
| 位对齐（低字节恒 `0x00`） | ✅ 24bit 在 `[31:8]` | 一致 |
| 右声道恒为 0 | ✅ 声道选择正确 | 一致 |
| 直流偏置 | ≈ 350 | PICO: 364~424 |
| **响应声音** | ✅ 拍手峰值 **851** | PICO: 751 |
| **每帧 64 SCK** | ✅ **HW v2 与 v1 行为一致** | 之前未验证过 |

→ **阶段 1 的麦克风配置在 S3 上原样成立。**

> ⚠️ 排查中的两个教训：
> 1. **3V3 没接** 时表现为 `raw`/`rec` **精确全 0**。这个特征应直接指向
>    **供电**，而不是接线错或配置错。
> 2. **需要人配合的测试，窗口不该短于 60 秒。** 我曾连续三次用 15~20 秒窗口
>    得到"无响应"的**假阴性**，拉到 60 秒立刻成功。

---

## 6. 未验证项（不隐藏）

1. **经典 ESP32 路径没有硬件回归验证** —— 改动了 `i2s_mic.c` / `diag.c` 后，
   PICO 板当时未连接，所以只做了**编译验证**。改动是机械的（宏替换），
   风险低，但**下次接上 PICO 时应该重跑一次阶段 2 验收**作为回归测试。
2. **S3 上没有接麦克风** —— I²S 初始化成功只说明外设配置对了，
   **不等于能采到声音**。接上 ICS-43434 后必须重跑阶段 1 的诊断
   （`raw` 判定位对齐、`level` 验证响应）。
3. **S3 上 64 SCK/帧是否成立未验证** —— 这是 ICS-43434 的硬约束，
   在经典 ESP32（I2S HW v1）上已验证；S3 是 HW v2，理论上一致，
   但没有实测过。接上麦克风后用 `raw` / 示波器确认。
