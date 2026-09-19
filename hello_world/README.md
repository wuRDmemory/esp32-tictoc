# hello_world — ESP32-PICO-V3-02

ESP32-PICO 开发板的 hello_world 工程。同时做两件事：

1. **验证板子身份** — 打印芯片型号、核心数、硅片版本、真实 Flash 容量、**PSRAM 容量**、MAC 地址
2. **验证双向通信** — 串口回显；外加 `info` / `reset` 两个命令

---

## 板子事实（全部为实测，非推测）

```
$ esptool.py --port /dev/ttyUSB0 flash_id
Chip is ESP32-PICO-V3-02 (revision v3.1)
Features: WiFi, BT, Dual Core, 240MHz, Embedded Flash, Embedded PSRAM, ...
Manufacturer: 20    Device: 4017
Detected flash size: 8MB
```

| 项目 | 值 |
|---|---|
| 封装 | **ESP32-PICO-V3-02** —— SiP：芯片 + 8MB Flash + 2MB PSRAM + 晶振 封在一个 7×7mm 里 |
| 芯片 | **经典 ESP32**，Xtensa LX6 双核 —— **不是** S3/C3/C6 |
| Flash | **8 MB** |
| PSRAM | **2 MB**（eFuse 里标了 `Embedded PSRAM`） |
| 硅片版本 | **v3.1** |
| USB-UART | **CP2102N** → Linux 侧 `/dev/ttyUSB0` |
| 板载 LED | **无用户可控 LED**，只有一颗 5V 电源指示灯 —— 所以本工程不做 blink |

> ⚠️ **别被型号名骗了。** 官方文档说 ESP32-PICO-KIT V4/V4.1 用的是 **PICO-D4**（4MB Flash、无 PSRAM、芯片 v1.0/v1.1）。但这块板子实测三条**全不符**——国内大量第三方板叫"PICO-KIT"却装 V3-02 模块。
>
> **拿到任何板子，先跑 `esptool.py flash_id`。** 它直接读 eFuse，是权威身份来源；丝印和卖家标题都不可信。
>
> 另一个线索：启动日志里这行警告就说明固件头配小了 ——
> `W spi_flash: Detected size(8192k) larger than the size in the binary image header(4096k)`

---

# 🔁 重启后要做什么

**这一节是最常查的。** 先看哪些状态会丢：

| 状态 | Windows 重启后 | WSL 重启后 | 存在哪 |
|---|---|---|---|
| usbipd 服务 | ✅ 自动启动 | ✅ 不受影响 | Windows 服务，`Automatic` |
| `bind` 绑定 | ❓ 未验证 | ✅ 不受影响 | Windows 侧 |
| **`attach`** | ❌ 丢失 | ❌ 丢失 | 内存态 |
| **cp210x 模块** | ❌ 丢失 | ❌ 丢失 | WSL 内核，随实例消失 |
| **`chmod 666`** | ❌ 丢失 | ❌ 丢失 | 设备节点，每次重建 |
| `oesp` 环境 | ❌ 丢失 | ❌ 丢失 | 每个终端独立 |

## 场景 A：Windows 重启（WSL 跟着重启）

### 第 1 步 — 检查设备是否还处于共享状态

打开 **Windows PowerShell**：

```powershell
usbipd list
```

找到 `10c4:ea60` 那行，看 **STATE** 列：

- 显示 **`Shared`** 或 **`Shared (forced)`** → ✅ 绑定还在，**跳到第 2 步**
- 显示 **`Not shared`** → ❌ 绑定丢了，先执行下面这条（**管理员** PowerShell，会弹 UAC）：

```powershell
usbipd bind --force --busid 4-4
```

> **为什么必须加 `--force`**：你的系统装了 UsbDk 过滤驱动，它和 usbipd 抢同一层，不加会被拒绝。
>
> **代价**（usbipd 官方原话）：`"Force binding; the host cannot use the device"`
> —— 强制绑定后 **Windows 永久用不了这块设备**，连 `detach` 都不还，只有 `unbind` 能还。
> **这就是 Windows 里看不到 COM3 的根本原因，是预期行为不是故障。**

### 第 2 步 — 把设备接进 WSL

**普通 PowerShell 即可**（不需要管理员）：

```powershell
usbipd attach --wsl --busid 4-4
```

> **为什么要这步**：`attach` 是内存态，重启必丢。
>
> **⚠️ BUSID 会变**：`4-4` 是「第 4 号 USB 控制器第 4 个口」的物理地址。**换一个 USB 插口编号就变**，必须重新 `usbipd list` 查。插同一个口则不变。

### 第 3 步 — 加载内核驱动

回到 **WSL 终端**：

```bash
sudo modprobe cp210x
```

> **为什么需要**：`/dev/ttyUSB0` 这个设备节点是 **cp210x 内核驱动**创建的，不是 USB 子系统自动建的。
>
> **为什么不会自动加载**：Linux 正常靠 **udev** 在热插拔时读设备 modalias 自动 `modprobe`。而这台 WSL 的 PID 1 是 `init` 不是 systemd，**没有 udev daemon**，所以得手动。
>
> **频率**：每个 WSL 实例一次就够 —— 模块加载后会一直留着。

### 第 4 步 — 放开设备权限

```bash
sudo chmod 666 /dev/ttyUSB0
```

> **为什么需要**：同理没有 udev。内核 devtmpfs 建节点时用默认权限 `root:root 0600`，只有 root 能读写。udev 在的话会按 Ubuntu 自带规则把它归到 `dialout` 组（你已在该组），自动就可用。
>
> **频率**：**每次 attach 都要重来** —— 设备节点每次重建，权限跟着复位。

### 第 5 步 — 激活 IDF 环境

```bash
oesp
```

> **为什么要**：`idf.py` 依赖一堆环境变量（`IDF_PATH`、工具链 PATH、Python venv）。`oesp` 会先 `conda deactivate` 再 source `export.sh`，避开 conda base(py3.12) 和 IDF 抢解释器的问题。
>
> **频率**：**每个新终端都要**，不会自动激活。

### 第 6 步 — 编译烧录看打印

```bash
cd ~/Workspace/projects/esp32/hello_world
idf.py -p /dev/ttyUSB0 flash monitor
```

退出监视器 **`Ctrl + ]`**。

## 场景 B：只重启 WSL（Windows 没重启）

**步骤完全一样**，从第 1 步走。可能省掉第 1 步（`bind` 在 Windows 侧不受 WSL 影响），但**看一眼确认比猜快**。

## 场景 C：只是新开一个终端

**只需要 `oesp`。** 设备、驱动、权限都还在。

## 场景 D：拔掉 USB 线再插回（都没重启）

| 步骤 | 需要吗 | 说明 |
|---|---|---|
| `bind` | ❌ 不需要 | Windows 侧状态没变 |
| `attach` | ✅ **需要** | 拔线等于 detach，得重新 attach |
| `modprobe` | ❌ 不需要 | 模块还在内核里 |
| `chmod` | ✅ **需要** | 节点重建了 |
| `oesp` | 看情况 | 终端没关就不用 |

> ⚠️ 插**不同的 USB 口**要重新 `usbipd list` 查 BUSID。

## 最小记忆版

```
Windows:  usbipd list  →  usbipd attach --wsl --busid 4-4
WSL:      sudo modprobe cp210x
          sudo chmod 666 /dev/ttyUSB0
          oesp
```

---

# 用法

### 编译

```bash
oesp
cd ~/Workspace/projects/esp32/hello_world
idf.py build
```

### 一键端到端验证（不用手动敲）

```bash
oesp
python ~/Workspace/projects/esp32/test_board.py
```

它会硬复位板子、抓启动日志、**发数据验回显**、测 `info` 命令，最后逐项打勾。

> ⚠️ **它和 `idf.py monitor` 抢串口** —— 监视器开着时跑它会报 `multiple access on port`。先 `Ctrl+]` 退出监视器。

---

## 预期输出

```
Hello world! 这是 ESP32-PICO-V3-02

================ 板子信息 ================
芯片型号   : esp32
CPU 核心数 : 2
硅片版本   : v3.1
射频功能   : WiFi BT BLE
Flash 容量 : 8 MB (封装内)
PSRAM 容量 : 2 MB
WiFi MAC   : 00:4B:12:xx:xx:xx
IDF 版本   : v5.5.5
空闲堆内存 : 2395779 字节
==========================================
```

**怎么算真的对：**

| 判据 | 说明 |
|---|---|
| `Flash 容量 : 8 MB` | 8MB 配置生效；显示 4MB 说明固件头配小了 |
| `PSRAM 容量 : 2 MB` | 由 `esp_psram_get_size()` 上报，不是读配置 |
| **`空闲堆内存` ≈ 2.3 MB** | **最硬的证据** —— PSRAM 未启用时只有 ~295 KB。涨到 2MB+ 才说明 PSRAM 真的并入了堆、能分配使用 |
| 敲字能回显 | USB↔板子双向都通 |

---

## 排错

### 逐层定位设备问题

按顺序查，每一步的根因都不同：

| 现象 | 根因 | 解法 |
|---|---|---|
| Windows 里都没有 COM 口 | **正常** —— `bind --force` 的必然结果 | 不用修。要用就 attach 进 WSL；要给 Windows 就 `usbipd unbind --busid 4-4` |
| Windows 有 COM 口，WSL 里没有 | 没 attach 进 WSL | `usbipd attach --wsl --busid <BUSID>` |
| attach 了但还是没有 `/dev/ttyUSB0` | **WSL 无 udev，热插拔不自动加载内核驱动** | `sudo modprobe cp210x` |
| 有 `/dev/ttyUSB0` 但读不了 | **WSL 无 udev，节点默认 `root:root 0600`** | `sudo chmod 666 /dev/ttyUSB0` |
| `multiple access on port` | 别的程序占着串口（通常是 `idf.py monitor`） | `Ctrl+]` 退出监视器 |

确认驱动是否已绑定（dmesg 才是权威，`readlink` 查 `1-1` 会看到父设备的通用驱动，别被误导）：
```bash
dmesg | grep cp210x
```

确认 Windows 侧设备归谁管：
```bash
powershell.exe -NoProfile -Command "Get-PnpDevice | Where-Object { \$_.InstanceId -like '*10C4*' } | Format-List FriendlyName, Status"
```
显示 `USBIP Shared Device` = 被 usbipd 接管了，COM 口必然消失。

### 板子反复重启，串口刷错误日志

配置和硬件不符。最常见是 `CONFIG_SPIRAM=y` 但板子实际没有 PSRAM。改回 `n` 重刷。

### 敲键盘板子没反应，要按好几次回车

`console_enable_blocking_read()` 没生效（UART 驱动没装上）。
串口日志里会有 `!! UART 驱动安装失败` 的提示。

### 想恢复出厂配置

```bash
rm -rf build sdkconfig
idf.py set-target esp32    # 会从 sdkconfig.defaults 重新生成
```

---

## 改配置的正确姿势

改 `sdkconfig.defaults`，**不要**直接改 `sdkconfig`。

`sdkconfig` 是构建产物，里面混着 IDF 的全部默认值；`sdkconfig.defaults` 才是你的意图。
但注意：**`sdkconfig` 里已存在的旧值不会自动被 `sdkconfig.defaults` 覆盖** ——
所以改完 `.defaults` 要 `rm sdkconfig` 重新生成，否则会以为改了却没生效。

图形化配置用 `idf.py menuconfig`。存盘写进 `sdkconfig`；想长期保留就手动搬回 `sdkconfig.defaults`。

---

## 当前构建配置摘要

| 配置项 | 值 | 为什么 |
|---|---|---|
| `CONFIG_IDF_TARGET` | `esp32` | V3-02 里是经典 ESP32 |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `8MB` | 实测 8MB；**IDF 对 esp32 默认只有 2MB** |
| `CONFIG_SPIRAM` | `y` | 实测有 2MB PSRAM；模式/型号/速度走 IDF 默认（quad/auto/40M），正好匹配 |
| `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` | `y` | app 分区 1MB → **1500K**，给 WiFi+BLE 留余量 |
| `CONFIG_ESP_CONSOLE_UART_NUM` | `0` | UART0 → CP2102N → `/dev/ttyUSB0` |

> 8MB Flash 目前只用了 1.5MB（app）+ 28KB（nvs/phy）。剩下约 6.5MB 闲置。
> 以后要存文件/日志可以加 `spiffs` 或 `fatfs` 分区，改自定义分区表 CSV 即可。

---

## 还能优化什么

第 3、4 步（两条 sudo）同源于**缺 udev**，两条根治路径：

| 方案 | 消掉哪几步 | 代价 |
|---|---|---|
| `/etc/wsl.conf` 加 `[boot] command = /sbin/modprobe cp210x` | 只消第 3 步 | 零风险，改一次配置 |
| 启用 systemd（udev 就活了） | **第 3、4 步全消** | 要 `wsl --shutdown` 重启，且 docker/ROS 也会切到 systemd 管理 |

---

## 目录结构

```
esp32/
├── CLAUDE.md               # 给 Claude Code 的项目说明
├── test_board.py           # 端到端验证脚本
└── hello_world/
    ├── CMakeLists.txt      # 工程入口，几乎不用动
    ├── sdkconfig.defaults  # 板级配置 —— 改配置来这里
    ├── README.md           # 本文件
    ├── sdkconfig           # 自动生成，可随时删
    ├── build/              # 构建产物，可随时删
    └── main/
        ├── CMakeLists.txt  # 声明源文件和依赖的组件
        └── hello_world_main.c  # 全部业务代码
```
