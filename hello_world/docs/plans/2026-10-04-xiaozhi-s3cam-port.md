# 小智 AI（xiaozhi-esp32）移植到 ESP32-S3-CAM —— 阶段 1：编译烧录

> **日期**：2026-10-04
> **目标板**：ESP32-S3-CAM (AI-Thinker 映射, N16R8) + ICS-43434
> **上游**：<https://github.com/78/xiaozhi-esp32>，本地 `xiaozhi-esp32/`（分支 `s3cam-port`）
> **代码位置**：`esp32/xiaozhi-esp32/`（已加进父仓库 `.gitignore`，它是独立 git 仓库）

**Goal:** 小智固件在 S3-CAM 上跑起来 —— **编译、烧录、连 WiFi、与 xiaozhi.me 握手拿到验证码、麦克风采音正常**。

**为什么到"能听"为止**：用户目前**没有功放也没有喇叭**，出声这件事在硬件到位前无法验证。本阶段不追。

---

## 1. 决策记录

### D-x1：走 xiaozhi **v2.3.0** + 现有 **ESP-IDF v5.5.5**

上游 IDF 要求的时间线（实测各 tag 的 `main/idf_component.yml` + README）：

| tag | IDF 要求 |
|---|---|
| **v2.5.0 / main** | **>= 6.0.1**，README 明写「不再支持 ESP-IDF 5.x」 |
| v2.4.2 / v2.4.0 | >= 5.5.2（主线已迁 IDF 6.0，5.5 仅为旧板保留） |
| **v2.3.0** | **5.4+** ← 本机 5.5.5 直接可用 |
| v2.1.0 / v1.9.4 | >= 5.4.0 |

选 v2.3.0 而不是 v2.4.2：后者 README 已声明主线迁到 IDF 6.0，5.5 只保旧板；
v2.3.0 的 README 干净地写「5.4 或以上」。两者只差 4 天，功能上无实质差别。

**代价（要记住）**：落后上游约 2.5 个月。日后若想跟进，需要装 IDF 6.1（约 4GB 工具链）
并做一次迁移。本机 voice_notes 继续用 IDF 5.5.5，两套环境可并存。

### D-x2：新建独立板级目录，**不改上游任何板子**

上游 `AGENTS.md` 与 `docs/custom-board_zh.md` 都**明确禁止**改现有板子的 IO 配置：
每块板有唯一的 OTA 通道标识，改了会导致 **OTA 把本固件刷成官方同型号固件**。

→ 新建 `main/boards/esp32-s3-cam-ics43434/`。

### D-x3：基线用 `bread-compact-wifi` 的思路（最小），不用 `bread-compact-wifi-s3cam`

上游那块 s3cam 板虽然名字像，但**接线完全不同**：

| | 上游 `bread-compact-wifi-s3cam` | 本板 |
|---|---|---|
| 麦克风 | 1 / 2 / 42 | **14 / 21 / 47** |
| 扬声器 | 39 / 40 / 41 | **41 / 42 / 43**（预留） |
| LCD | ST7789，占 **19/20/38/45/47/21** | **无** |
| LAMP | `LAMP_GPIO = 14` | 无 |

它的 LCD 与 LAMP 引脚**与我们的麦克风在 47、21、14 上直接撞车**。所以另起炉灶。

### D-x4：服务器用官方 **xiaozhi.me**

固件是纯客户端，没有免服务器的本地模式。启动流程：
`ota.cc` 请求 OTA 端点（默认 `https://api.tenclass.net/xiaozhi/ota/`）→
响应里下发 websocket/mqtt 地址 + `activation.code` → 用户在 xiaozhi.me 控制台绑定。

---

## 2. 两个实测发现（都来自源码，不是推测）

### ⚠️ 发现 1：ICS-43434 在 xiaozhi 的 codec 下**根本不出数据**

`main/audio/codecs/no_audio_codec.cc:46-50`：

```c
.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
.slot_mode      = I2S_SLOT_MODE_MONO,     // ← 32 SCK/帧
.slot_mask      = I2S_STD_SLOT_LEFT,
```

**MONO + 32bit = 每 WS 帧 32 个 SCK**。而本项目的 `CLAUDE.md` 与
`prd.md` §6 明确记录：**ICS-43434 要求每帧正好 64 个 SCK**，必须 `STEREO` + 32bit。
用上游配置的症状不是"音质差"，是**麦克风完全没数据**。

→ 必须自带 codec。这不是优化，是**能不能用的分界线**。

### ⚠️ 发现 2：验证码**拿不到**（无屏 + 无喇叭）

`application.cc` 的 `ShowActivationCode()` 只把验证码交给两条路：
**屏幕显示** 和 **语音逐位播报**（`Alert()` 里 `ESP_LOGW` 打的是 `message` 提示语，
**不是 `code` 本身**）。

本板既没屏幕也没扬声器 → **用户永远看不到那 6 位验证码 → 设备永远激活不了**。

→ 在 `ShowActivationCode()` 开头加一行 `ESP_LOGW` 把码打到串口。
**这是本板的必需品，不是调试残留**，已在代码注释里写明。

---

## 3. 引脚方案

排针引出范围（用户实测）：**GPIO 3~21** 与 **GPIO 39~48**。
⚠️ 注意 **GPIO38 不在引出范围内**，所以 SD 的 38/39/40 只有 39/40 可用。

| 用途 | GPIO | 依据 |
|---|---|---|
| MIC BCLK / WS / DIN | **14 / 21 / 47** | voice_notes 阶段 1 **已验证**，原样沿用 |
| SPK BCLK / LRCK / DOUT | **41 / 42 / 43** | 预留。41/42=JTAG（不用外接调试器）；43=UART0 TX（console 走 USB 故空闲） |
| BOOT 按键 | 0 | 待实机确认 |

**必须避开**：19/20(USB D±)、26–37(Flash+八线 PSRAM)、4–13+15–18(摄像头)、
38–40(SD)、45/46(strapping)、48(板载 LED)、0/3(strapping，0 用作按键输入)

⚠️ **摄像头引脚绝对不能挪用**：VSYNC(6)/HREF(7) 是摄像头**输出**，
抢用会让两个器件同时驱动同一根线。

---

## 4. 改动的文件

**新建**（`xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/`）：

| 文件 | 职责 |
|---|---|
| `config.h` | 引脚与采样率，含大量"为什么不能用某个脚"的说明 |
| `config.json` | 构建脚本用的板级元数据（v2.3.0 格式，无 `type` 字段） |
| `ics43434_codec.h/.cc` | ★ **64 SCK/帧 + `>>16` 的专用 codec** |
| `esp32_s3_cam_board.cc` | 板级类，`DECLARE_BOARD` |
| `README.md` | 板级说明 |

**修改**（注册链，三处，缺一不可）：

| 文件 | 改动 |
|---|---|
| `main/Kconfig.projbuild` | 加 `config BOARD_TYPE_ESP32_S3_CAM_ICS43434` |
| `main/CMakeLists.txt` | 加 `elseif` 分支 → `set(BOARD_TYPE "esp32-s3-cam-ics43434")` + 字体 |
| `main/application.cc` | `ShowActivationCode()` 加一行打印验证码（见发现 2） |

**父仓库**：`.gitignore` 加 `xiaozhi-esp32/`（避免嵌套 git 仓库）。

---

## 5. 验收结果（2026-10-04 实测通过 ✅）

**不接受"编译过了"当结论。** 全部判据都拿到了串口日志证据，**错误数 0**：

| # | 判据 | 证据 |
|---|---|---|
| 1 | 启动无 panic | `Board: UUID=d7e04db3-… SKU=esp32-s3-cam-ics43434` |
| 2 | **自定义 codec 生效** | `Ics43434Codec: ICS-43434 codec ready: mic 16000 Hz (STEREO/32bit = 64 SCK/frame, >>16), spk 24000 Hz` |
| 3 | WiFi 配网 + 连接 | 热点 `Xiaozhi-C391` → 连上家里 WiFi，`Got IP: 192.168.3.62` |
| 4 | 与服务器握手 | `Established new connection to api.tenclass.net:443`，证书校验通过 |
| 5 | **拿到激活码** | `================ 激活码 / ACTIVATION CODE: 189501 ================`（靠我们打的补丁，见发现 2） |
| 6 | 激活成功 | `Ota: Activation successful` → `MQTT: Connected to endpoint` → `State: activating -> idle` |
| 7 | **麦克风采音正常** | `Application: Wake word detected: 你好小智` |
| 8 | STT 识别正确 | `Display: Role:user` → `你好小智` |
| 9 | 服务器对话 | `Application: << 嗨～你好呀！` |
| 10 | **BOOT 按键 = GPIO0** | 按下后 `StateMachine: State: idle -> connecting`，**且此前无 `Wake word detected`** —— 证明是按键路径而非唤醒词路径（用户确认确实按了） |

### 两条关键结论

**① `>>16` 是对的，不需要向 `>>12` 靠拢。**
事前担心过：上游用 `>>12`（16 倍增益，按 INMP441 调），我们按数学对齐用 `>>16`，
而本模块灵敏度偏低 —— 会不会音量不够触发 WakeNet？**实测证明不会**，一次唤醒成功。
所以**没有**照抄上游那个值。（`ICS43434_SHIFT` 保留为独立宏，日后要调仍是一处改动。）

**② 串口补丁确实是必需品，不是洁癖。**
若没有 `ShowActivationCode()` 里那行 `ESP_LOGW`，本板无屏无喇叭，
验证码无处可去 —— 设备**永远无法激活**。

### 仍未验证

- **扬声器**：硬件未到位。`spk 24000 Hz` 初始化成功、`Set output enable to true` 不崩，
  但**引脚 41/42/43 从未接过真实负载**，等 MAX98357A 到位才能验。
- 摄像头：本阶段未启用。

---

## 6. 尚未解决 / 下一步

- **扬声器引脚从未在真机验证**（无硬件）。买到 MAX98357A 后要重新确认 41/42/43 可用。
- **唤醒稳定性未测**：只测到 1~2 次成功唤醒，足以判定"打通"，但唤醒率与唤醒距离
  （本模块灵敏度低于规格）都还没量过，属于调优范畴。
- **摄像头**：引脚已记录在 `config.h`，但板级代码**未初始化**。这是第二步的工作。
  上游有现成的摄像头抽象（`main/boards/common/camera.h`、`esp32_camera.cc`、
  `esp_video.cc`），且有 17 块板子实现了它 —— 不用从零写。
- **DC 阻断**：`ics43434_codec.cc` 暂未做（voice_notes 里有，实测该模块有 1% 量级的
  直流偏置）。若发现唤醒/VAD 不灵敏，这是第一个该加的东西。
- **未验证 `>>16` 的实际音量是否够** —— 该模块灵敏度低于规格（见 `CLAUDE.md`）。
