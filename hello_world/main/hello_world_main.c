/*
 * ESP32-PICO-KIT V4 hello_world
 *
 * 板子 : ESP32-PICO-KIT V4   (ESP32-PICO-D4 封装, 经典 ESP32 双核)
 *        4MB Flash / 无 PSRAM / 板载 CP2102 USB-UART -> /dev/ttyUSB0
 *
 * 做两件事:
 *   1) 开机打印芯片型号 / 核心数 / 硅片版本 / 真实 Flash 容量 / MAC
 *      —— 用来确认固件真的跑在你以为的那块芯片上
 *   2) 串口回显 + 两个内置命令
 *      —— 用来验证 USB <-> 板子 的双向通信
 *
 * 编译: idf.py build
 * 烧录: idf.py -p /dev/ttyUSB0 flash monitor   (Ctrl+] 退出监视器)
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_psram.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#define PROMPT       "esp32> "
/* 注意别叫 LINE_MAX —— 那是 POSIX <limits.h> 的系统宏, 重名会触发 redefine 警告 */
#define CMD_LINE_MAX 128

/* ------------------------------------------------------------------ */
/* 板子信息                                                            */
/* ------------------------------------------------------------------ */
static void print_board_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    printf("\n");
    printf("================ 板子信息 ================\n");
    printf("芯片型号   : %s\n", CONFIG_IDF_TARGET);
    printf("CPU 核心数 : %d\n", chip_info.cores);

    unsigned major = chip_info.revision / 100;
    unsigned minor = chip_info.revision % 100;
    printf("硅片版本   : v%u.%u\n", major, minor);

    printf("射频功能   : %s%s%s%s\n",
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
           (chip_info.features & CHIP_FEATURE_BT)       ? "BT "   : "",
           (chip_info.features & CHIP_FEATURE_BLE)      ? "BLE "  : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4 " : "");

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("Flash 容量 : %" PRIu32 " MB (%s)\n",
               flash_size / (1024 * 1024),
               (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "封装内" : "外挂");
    } else {
        printf("Flash 容量 : 读取失败\n");
    }

    /* PSRAM: V3-02 封装内有 2MB。只有开了 CONFIG_SPIRAM 才能访问到。
     * 这里用程序自己上报, 而不是相信配置 —— 配置写了不代表芯片真有。 */
    if (esp_psram_is_initialized()) {
        printf("PSRAM 容量 : %u MB\n",
               (unsigned)(esp_psram_get_size() / (1024 * 1024)));
    } else {
        printf("PSRAM 容量 : 未启用\n");
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        printf("WiFi MAC   : %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    printf("IDF 版本   : %s\n", esp_get_idf_version());
    printf("空闲堆内存 : %" PRIu32 " 字节\n", esp_get_free_heap_size());
    printf("==========================================\n\n");
}

/* ------------------------------------------------------------------ */
/* 把 console 切到阻塞读                                              */
/*                                                                    */
/* 默认 console 是非阻塞的, 直接 fgetc/fgets 会立刻返回 EOF,          */
/* 表现出来就是"我敲键盘板子没反应"。装 UART 驱动 + 告诉 VFS 用驱动    */
/* 之后, fgets 才会阻塞等待输入, 用起来才像个终端。                    */
/* ------------------------------------------------------------------ */
static void console_enable_blocking_read(void)
{
    const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

    /* 关键: 关掉 stdio 缓冲, 否则要回车好几下才有反应 */
    setvbuf(stdin, NULL, _IONBF, 0);

    esp_err_t err = uart_driver_install(port, 256, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* 装不上就退回非阻塞模式: 回显仍然可用, 只是要按回车 */
        printf("!! UART 驱动安装失败 (%s), 退回非阻塞模式\n",
               esp_err_to_name(err));
        return;
    }

    uart_vfs_dev_use_driver(port);
    /* 串口终端习惯: 把收到的 \r 当行结束, 输出 \n 时自动补 \r */
    uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(port, ESP_LINE_ENDINGS_CRLF);
}

/* ------------------------------------------------------------------ */
/* 主程序                                                              */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    console_enable_blocking_read();

    printf("\nHello world! 这是 ESP32-PICO-KIT V4\n");
    print_board_info();

    printf("输入任意内容并回车, 板子会原样回给你 —— 这就是双向通信的验证。\n");
    printf("内置命令: info = 重看板子信息 | reset = 重启板子\n\n");
    fputs(PROMPT, stdout);
    fflush(stdout);

    static char line[CMD_LINE_MAX];

    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* 非阻塞模式下的空读。让出 CPU, 免得空转把 CPU 跑满 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 去掉行尾的 \r \n */
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "info") == 0) {
            print_board_info();
        } else if (strcmp(line, "reset") == 0) {
            printf("重启中...\n");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else if (line[0] != '\0') {
            printf("回显: %s\n", line);
        }

        fputs(PROMPT, stdout);
        fflush(stdout);
    }
}
