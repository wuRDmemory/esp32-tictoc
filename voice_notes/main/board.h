/*
 * board.h —— 板级差异的**唯一出处**
 *
 * 为什么需要它：PICO-V3-02 和 S3-CAM **没有一个参数是相同的** ——
 * 引脚、时钟源、PSRAM 模式、console 通路全都不同。而且其中两条是**硬约束**：
 *
 *   - GPIO22/25 在 S3 上【物理不存在】（S3 只有 0–21、26–48）
 *   - I2S_CLK_SRC_APLL 在 S3 上【不存在】
 *
 * 所以"改几个数字让两边通用"是行不通的：CAM 板能用的引脚在经典 ESP32 上
 * 会撞 SPI Flash（GPIO6–11），反之亦然。必须按板编译期选择。
 *
 * 选择方式：
 *     idf.py menuconfig  →  "Target board"
 * 默认值按 IDF target 自动选（见 main/Kconfig.projbuild），所以
 * `idf.py set-target esp32s3` 之后通常不用手动选。
 *
 * ⚠️ 新增板子时：先在 board.h 加一段，再在 Kconfig.projbuild 加一个选项。
 *    忘了其中一处会得到 #error 而不是静默用错引脚 —— 这是刻意的。
 */
#pragma once

#include "sdkconfig.h"
#include "driver/i2s_std.h"   /* I2S_CLK_SRC_* */

/* ================================================================== */
/* ESP32-PICO-V3-02（经典 ESP32，Xtensa LX6）                           */
/*                                                                    */
/* 阶段 1/2 已验证通过。所有参数来自 docs/hardware.md §7 的【实测】，   */
/* 不是推导值 —— 改之前先读那一节。                                     */
/* ================================================================== */
#if defined(CONFIG_BOARD_PICO_V3_02)

#define BOARD_NAME              "ESP32-PICO-V3-02"
#define BOARD_CHIP_FAMILY       "ESP32 (经典, LX6)"

/* I²S 接线：→ ICS-43434 的 WS/SCK/SD
 * 避开了 GPIO6-11(Flash) / 0,2,12,15(strapping) / 1,3(UART) */
#define BOARD_I2S_BCLK          26
#define BOARD_I2S_WS            25
#define BOARD_I2S_DIN           22

/* 经典 ESP32 有 APLL —— 音频时钟更精确（NFR-3），实测可用 */
#define BOARD_I2S_CLK_SRC       I2S_CLK_SRC_APLL
#define BOARD_I2S_CLK_SRC_NAME  "APLL"

#define BOARD_PSRAM_MB          2

/* 板子没有用户可控 LED（只有 5V 电源指示灯）—— 别写 blink 示例 */
#define BOARD_HAS_USER_LED      0

/* 经典 ESP32 不支持 ESP-SR 的 AFE/VAD/WakeNet（见 docs/wakeword-research.md），
 * 所以音频前端只能是直通，VAD/唤醒由 PC 承担 */
#define BOARD_HAS_ESPSR         0

/* console 走 UART0（经板载 CP2102N）。引脚和波特率在 sdkconfig.defaults.esp32 里 */
#define BOARD_CONSOLE_KIND      "UART0 @ 921600 (CP2102N)"

/* ================================================================== */
/* ESP32-S3-CAM（AI-Thinker 映射，N16R8）                              */
/*                                                                    */
/* 2026-10-04 实测：Flash 16MB / PSRAM 8MB 八线 / 无 APLL / 原生 USB。  */
/* 首次上板踩的坑见 docs/s3-bringup.md。                                */
/*                                                                    */
/* ⚠️ 曾误当作 ESP32-S3-DevKitC-1 处理 —— 型号是用户后来更正的。        */
/*    CAM 板与 DevKitC 的关键差别：**摄像头吃掉十几个 GPIO**。          */
/* ================================================================== */
#elif defined(CONFIG_BOARD_S3_CAM)

#define BOARD_NAME              "ESP32-S3-CAM"
#define BOARD_CHIP_FAMILY       "ESP32-S3 (LX7)"

/* ⚠️ 引脚选取 —— 这块板子能用的引脚非常少，逐条说明为什么：
 *
 *   摄像头占用（AI-Thinker 映射，丝印为 ESP32-S3-CAM 的板子实测匹配）：
 *     D0=11 D1=9 D2=8 D3=10 D4=12 D5=18 D6=17 D7=16
 *     XCLK=15 SIOD=4 SIOC=5 VSYNC=6 HREF=7 PCLK=13
 *     → 即 4–13、15–18 全部被占
 *
 *   ❌ 绝对不能用摄像头引脚，不只是"被占用"那么简单：
 *      - VSYNC(6) / HREF(7) 是【摄像头输出】—— 我们当 WS/BCLK 用时
 *        两个器件同时驱动同一根线，信号打架且可能损伤引脚
 *
 *   ❌ 其他不可用：
 *      GPIO 0/3/45/46  strapping
 *      GPIO 19/20      原生 USB（D−/D+）
 *      GPIO 26–37      Flash + 八线 PSRAM
 *      GPIO 43/44      UART0
 *      GPIO 38/39/40   SD 卡
 *      GPIO 39–42      JTAG
 *      GPIO 2/48       板载 LED
 *
 *   ✅ 最终选 14 / 21 / 47 —— 这是推算出的**仅有的三个干净引脚**
 *      （已经用户确认都引到了排针上）。
 *
 *   ⚠️ 没有冗余：这三个任何一个被占用就得退到 41/42（失去 JTAG）
 *      或 38/39/40（牺牲 SD 卡）。 */
#define BOARD_I2S_BCLK          14
#ifndef BOARD_I2S_WS
#define BOARD_I2S_WS            21
#endif
#ifndef BOARD_I2S_DIN
#define BOARD_I2S_DIN           47
#endif

/* S3 没有 APLL（soc/esp32s3/clk_tree_defs.h 只有 PLL_F240M/PLL_F160M/XTAL）。
 * I2S_CLK_SRC_DEFAULT = PLL_F160M，160MHz / 1.024MHz = 156.25，
 * 靠小数分频，实测可用。 */
#define BOARD_I2S_CLK_SRC       I2S_CLK_SRC_DEFAULT
#define BOARD_I2S_CLK_SRC_NAME  "PLL_160M"

#define BOARD_PSRAM_MB          8

/* 板载 LED 通常是 WS2812（GPIO48），需要 RMT 驱动 —— 暂不使用 */
#define BOARD_HAS_USER_LED      0

/* S3 支持 ESP-SR 的 AFE + VAD + WakeNet9 —— 这正是换 S3 的动机 */
#define BOARD_HAS_ESPSR         1

/* console 走原生 USB 的 USB-Serial-JTAG：
 *   - cdc_acm 内建于 WSL 内核，【不需要 modprobe】
 *   - 下载模式与运行固件都是 303a:1001，PID 稳定 → 不用反复重绑
 *   - 烧录速度约 1337 kbit/s（CP2102N 只有 643） */
#define BOARD_CONSOLE_KIND      "USB-Serial-JTAG (原生 USB)"

#else
#error "未选择板子：idf.py menuconfig → Target board（默认值按 IDF target 自动选）"
#endif

/* ================================================================== */
/* 派生量：所有板子通用                                                */
/* ================================================================== */

/* 采样率与每帧样本数由麦克风决定，与板子无关 */
#define BOARD_SAMPLE_RATE       16000
#define BOARD_FRAMES_PER_READ   256

/* 校验：引脚不能重复（复制粘贴新板子配置时最容易犯的错） */
#if BOARD_I2S_BCLK == BOARD_I2S_WS || BOARD_I2S_BCLK == BOARD_I2S_DIN \
 || BOARD_I2S_WS == BOARD_I2S_DIN
#error "board.h: I²S 的三个引脚不能相同"
#endif
