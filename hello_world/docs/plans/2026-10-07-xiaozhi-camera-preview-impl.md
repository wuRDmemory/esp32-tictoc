# 摄像头实时预览 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 ESP32-S3-CAM 上，长按 BOOT 键在 xiaozhi UI 与摄像头全屏实时预览之间切换。

**Architecture:** 板级自管摄像头（直接用 `esp_camera` 原生 API，零改动上游共享文件）。
帧数据逐像素交换字节序后交给 **LVGL 顶层的一个全屏 `lv_image`**，由 LVGL 负责 SPI 完成同步
（**不绕过 LVGL、不用 `lvgl_port_stop/resume`** —— 理由见 spec §3.2/R2）。

**Tech Stack:** ESP-IDF v5.5.5 · `espressif/esp32-camera`（已在 `idf_component.yml`）· LVGL 9 · esp_lvgl_port

**Spec:** `hello_world/docs/plans/2026-10-07-xiaozhi-camera-preview.md`

## Global Constraints

- **代码位置**：`xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/`（submodule，分支 `s3cam-port`）
- **零改动上游共享文件**（`main/boards/common/`、`main/display/` 等一律不动）
- **板级目录的 `.c`/`.cc` 会被 `file(GLOB .../*.c)` 自动收进固件**（`main/CMakeLists.txt:881-888`，**非递归**）
  → 新增源码**不要**改 CMakeLists；但主机测试**必须放子目录**（如 `test/host/`）否则会被编进固件
- ⚠️ **新增源码后必须强制 reconfigure**（Task 1 实测踩到）：`file(GLOB)` 在 **configure 时**求值并固化进
  `build.ninja`，**`idf.py build` 不会重新 glob** → 新文件**静默不被编译**（构建照样成功，极具迷惑性）。
  新增文件后要跑一次 `idf.py reconfigure` 再 build。
  **判据**：`build/esp-idf/main/CMakeFiles/__idf_main.dir/boards/esp32-s3-cam-ics43434/` 下出现对应 `.obj`
  ⚠️ `idf.py reconfigure` 可能报 `components-file.espressif.com ... invalid or empty JSON` —— 那是
  **组件 registry 的网络瞬时故障，与改动无关**；只要随后 build 通过且 `.obj` 生成即正常
- **构建/烧写**：必须先 `oesp`；`IDF_PY_BUILD_JOBS=12 idf.py build`；`idf.py -p /dev/ttyACM0 flash`
- **串口**：`/dev/ttyACM0`，每次重新 attach 后需 `sudo chmod 666 /dev/ttyACM0`
- **抓日志**：`oesp && python xiaozhi_monitor.py -t 25`（仓库根目录）
- **提交**：本仓库的提交**需用户确认后执行**（不自动提交）；submodule 与父仓库要**分别提交**，且**先推 submodule**
- **注释用中文**；不要用 `LINE_MAX` 之类 POSIX 宏名

---

### Task 1: `camera_frame` 纯 C 逻辑 + 主机单测

**Files:**
- Create: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/camera_frame.h`
- Create: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/camera_frame.c`
- Test: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/test/host/test_camera_frame.c`

**Interfaces:**
- Consumes: 无
- Produces:
  - `int camera_frame_center_offset(int panel_width, int frame_width)` —— 装不下返回 `-1`
  - `void camera_frame_swap_bytes(uint16_t *dst, const uint16_t *src, size_t pixel_count)` —— 原地（`dst==src`）安全

- [ ] **Step 1: 写失败的主机测试**

创建 `test/host/test_camera_frame.c`（写法照抄 `voice_notes/test/host/test_audio_frame.c` 的 `CHECK` 宏风格）：

```c
/*
 * 主机侧单元测试 —— camera_frame.c 不依赖 ESP-IDF，能在 PC 上用 gcc 直接测。
 *
 * 编译运行：
 *   gcc -Wall -Wextra -o /tmp/t_camframe test/host/test_camera_frame.c \
 *       camera_frame.c -I. && /tmp/t_camframe
 */
#include <stdio.h>
#include <stdint.h>

#include "camera_frame.h"

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void test_center_offset(void)
{
    /* 本板真实参数：屏 284 宽、画面 240 宽 → 两侧各留 22 */
    CHECK(camera_frame_center_offset(284, 240) == 22);
    /* 等宽 → 无黑边 */
    CHECK(camera_frame_center_offset(240, 240) == 0);
    /* 奇数差 → 向下取整 */
    CHECK(camera_frame_center_offset(241, 240) == 0);
    /* 装不下 → -1（调用方必须挡住） */
    CHECK(camera_frame_center_offset(240, 320) == -1);
    /* 非法参数 → -1 */
    CHECK(camera_frame_center_offset(284, 0) == -1);
    CHECK(camera_frame_center_offset(0, 240) == -1);
}

static void test_swap_bytes(void)
{
    uint16_t buf[3] = { 0x1234, 0x00FF, 0xABCD };

    camera_frame_swap_bytes(buf, buf, 3);          /* 原地 */
    CHECK(buf[0] == 0x3412);
    CHECK(buf[1] == 0xFF00);
    CHECK(buf[2] == 0xCDAB);

    /* 交换两次 = 还原（幂等性锚点） */
    camera_frame_swap_bytes(buf, buf, 3);
    CHECK(buf[0] == 0x1234);
    CHECK(buf[1] == 0x00FF);
    CHECK(buf[2] == 0xABCD);

    /* 异地：src 不被改动 */
    uint16_t src[2] = { 0x1111, 0x2222 };
    uint16_t dst[2] = { 0, 0 };
    camera_frame_swap_bytes(dst, src, 2);
    CHECK(dst[0] == 0x1111 && dst[1] == 0x2222);
    CHECK(src[0] == 0x1111 && src[1] == 0x2222);
}

int main(void)
{
    printf("test_camera_frame\n");
    test_center_offset();
    test_swap_bytes();
    if (g_fail == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILED\n", g_fail);
    return 1;
}
```

- [ ] **Step 2: 运行测试，确认它失败**

```bash
cd xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434
gcc -Wall -Wextra -o /tmp/t_camframe test/host/test_camera_frame.c camera_frame.c -I. && /tmp/t_camframe
```

Expected: **编译失败** —— `camera_frame.h: No such file or directory`

- [ ] **Step 3: 写最小实现**

创建 `camera_frame.h`：

```c
#ifndef _CAMERA_FRAME_H_
#define _CAMERA_FRAME_H_

#include <stddef.h>
#include <stdint.h>

/* 计算画面在屏上居中时左侧黑边的宽度。
 * 装不下（或参数非法）返回 -1 —— 调用方必须挡住这种情况。 */
int camera_frame_center_offset(int panel_width, int frame_width);

/* RGB565 逐像素字节交换。
 * ⚠️ 为什么必需：上游 esp32_camera.cc 对每一帧都做 __builtin_bswap16
 *    且 swap_bytes_enabled_ 默认 true —— 见 spec §7.1。
 * 原地交换安全（dst == src 合法）。 */
void camera_frame_swap_bytes(uint16_t *dst, const uint16_t *src, size_t pixel_count);

#endif /* _CAMERA_FRAME_H_ */
```

创建 `camera_frame.c`：

```c
#include "camera_frame.h"

int camera_frame_center_offset(int panel_width, int frame_width)
{
    if (panel_width <= 0 || frame_width <= 0 || frame_width > panel_width) {
        return -1;
    }
    return (panel_width - frame_width) / 2;
}

void camera_frame_swap_bytes(uint16_t *dst, const uint16_t *src, size_t pixel_count)
{
    for (size_t i = 0; i < pixel_count; i++) {
        dst[i] = __builtin_bswap16(src[i]);
    }
}
```

- [ ] **Step 4: 运行测试，确认全绿**

```bash
gcc -Wall -Wextra -o /tmp/t_camframe test/host/test_camera_frame.c camera_frame.c -I. && /tmp/t_camframe
```

Expected: `ALL PASS`（退出码 0）

- [ ] **Step 5: 确认它被编进固件且构建通过**

```bash
oesp && cd xiaozhi-esp32 && IDF_PY_BUILD_JOBS=12 idf.py build 2>&1 | tail -3
```

Expected: `Project build complete.`
（`camera_frame.c` 在板级目录下，应被 glob 自动收录 —— 若报"未定义引用"，说明 glob 没收到，需检查文件名后缀）

- [ ] **Step 6: 提交（需用户确认）**

```bash
cd xiaozhi-esp32
git add main/boards/esp32-s3-cam-ics43434/camera_frame.h \
        main/boards/esp32-s3-cam-ics43434/camera_frame.c \
        main/boards/esp32-s3-cam-ics43434/test/host/test_camera_frame.c
git commit -m "摄像头预览：新增纯 C 帧处理逻辑（字节序交换 + 居中计算）+ 主机单测"
```

---

### Task 2: `camera_preview` 模块 —— 摄像头生命周期（含临时自检）

**Files:**
- Create: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/camera_preview.h`
- Create: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/camera_preview.cc`
- Modify: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/esp32_s3_cam_board.cc`

**Interfaces:**
- Consumes: `camera_frame_center_offset()`（Task 1）
- Produces:
  - `class CameraPreview`，构造 `CameraPreview(esp_lcd_panel_handle_t panel, int panel_width, int frame_width)`
  - `bool Start()` —— 失败返回 false 且无副作用
  - `void Stop()` —— 可重复调用
  - `bool IsRunning() const`

- [ ] **Step 1: 写头文件**

创建 `camera_preview.h`：

```cpp
#pragma once

#include <lvgl.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* 摄像头实时预览（全屏居中，两侧黑边）
 *
 * 设计见 hello_world/docs/plans/2026-10-07-xiaozhi-camera-preview.md
 * 上屏路径：帧数据交给 LVGL 顶层的全屏 lv_image，SPI 完成同步由 LVGL 负责
 *          （⚠️ 不要绕过 LVGL 直推面板 —— 完成回调被 esp_lvgl_port 占着，见 spec R2）。
 */
class CameraPreview {
public:
    CameraPreview(esp_lcd_panel_handle_t panel, int panel_width, int frame_width);
    ~CameraPreview();

    /* 进入预览。摄像头初始化失败时返回 false，且不留任何副作用。 */
    bool Start();
    /* 退出预览：停任务 → 删 image → 释放摄像头。可重复调用。 */
    void Stop();
    bool IsRunning() const { return running_; }

private:
    static void TaskEntry(void *arg);
    void Run();

    esp_lcd_panel_handle_t panel_;
    int panel_width_;
    int frame_width_;
    int x_offset_ = -1;

    volatile bool running_ = false;
    TaskHandle_t task_ = nullptr;

    uint8_t *swap_buf_ = nullptr;      /* 交换后的 RGB565 缓冲（PSRAM） */
    lv_obj_t *black_bg_ = nullptr;     /* 全屏黑底（补两侧黑边） */
    lv_obj_t *lv_image_ = nullptr;     /* 全屏画面 */
    lv_image_dsc_t img_dsc_ = {};      /* ⚠️ 必须是成员：LVGL 会引用它，栈变量会悬空 */
};
```

- [ ] **Step 2: 写实现（先只做生命周期，抓帧循环留到 Task 3）**

创建 `camera_preview.cc`：

```cpp
#include "camera_preview.h"
#include "camera_frame.h"
#include "config.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_camera.h>
#include <esp_lvgl_port.h>

#define TAG "CameraPreview"

CameraPreview::CameraPreview(esp_lcd_panel_handle_t panel, int panel_width, int frame_width)
    : panel_(panel), panel_width_(panel_width), frame_width_(frame_width) {
    x_offset_ = camera_frame_center_offset(panel_width_, frame_width_);
    if (x_offset_ < 0) {
        ESP_LOGE(TAG, "预览宽 %d 装不进屏宽 %d", frame_width_, panel_width_);
    }
}

CameraPreview::~CameraPreview() {
    Stop();
}

bool CameraPreview::Start() {
    if (running_) {
        return true;
    }
    if (x_offset_ < 0) {
        return false;                      /* 构造函数已报过错 */
    }

    camera_config_t cfg = {};
    cfg.ledc_channel  = LEDC_CHANNEL_0;    /* S3 的 XCLK 不用 LEDC，但字段要填 */
    cfg.ledc_timer    = LEDC_TIMER_0;
    cfg.pin_d0        = CAMERA_PIN_D0;
    cfg.pin_d1        = CAMERA_PIN_D1;
    cfg.pin_d2        = CAMERA_PIN_D2;
    cfg.pin_d3        = CAMERA_PIN_D3;
    cfg.pin_d4        = CAMERA_PIN_D4;
    cfg.pin_d5        = CAMERA_PIN_D5;
    cfg.pin_d6        = CAMERA_PIN_D6;
    cfg.pin_d7        = CAMERA_PIN_D7;
    cfg.pin_xclk      = CAMERA_PIN_XCLK;
    cfg.pin_pclk      = CAMERA_PIN_PCLK;
    cfg.pin_vsync     = CAMERA_PIN_VSYNC;
    cfg.pin_href      = CAMERA_PIN_HREF;
    cfg.pin_sccb_sda  = CAMERA_PIN_SIOD;   /* ⚠️ 不指定 sccb_i2c_port → 让组件自建 I2C，
                                            *    避免与现有 I2C 总线冲突 */
    cfg.pin_sccb_scl  = CAMERA_PIN_SIOC;
    cfg.pin_pwdn      = CAMERA_PIN_PWDN;   /* NC：原理图确认经 1K 下拉，常使能 */
    cfg.pin_reset     = CAMERA_PIN_RESET;  /* NC：接 EN 网络 */
    cfg.xclk_freq_hz  = XCLK_FREQ_HZ;
    cfg.pixel_format  = PIXFORMAT_RGB565;
    cfg.frame_size    = FRAMESIZE_240X240;
    cfg.jpeg_quality  = 12;                /* RGB565 下无用，但字段要填 */
    cfg.fb_count      = 2;                 /* 双缓冲 */
    cfg.fb_location   = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init 失败: %s", esp_err_to_name(err));
        return false;
    }

    const size_t bytes = (size_t)frame_width_ * frame_width_ * sizeof(uint16_t);
    swap_buf_ = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (swap_buf_ == nullptr) {
        ESP_LOGE(TAG, "帧缓冲分配失败 (%u 字节)", (unsigned)bytes);
        esp_camera_deinit();
        return false;
    }

    img_dsc_.header.cf    = LV_COLOR_FORMAT_RGB565;
    img_dsc_.header.w     = frame_width_;
    img_dsc_.header.h     = frame_width_;
    img_dsc_.header.stride = frame_width_ * 2;
    img_dsc_.data         = swap_buf_;
    img_dsc_.data_size    = bytes;

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "取 LVGL 锁失败");
        heap_caps_free(swap_buf_);
        swap_buf_ = nullptr;
        esp_camera_deinit();
        return false;
    }
    /* 全屏黑底：补两侧黑边（否则黑边处会透出底下的 UI） */
    black_bg_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(black_bg_);
    lv_obj_set_size(black_bg_, panel_width_, frame_width_);
    lv_obj_set_style_bg_color(black_bg_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(black_bg_, LV_OPA_COVER, 0);
    lv_obj_center(black_bg_);

    lv_image_ = lv_image_create(lv_layer_top());
    lv_image_set_src(lv_image_, &img_dsc_);
    lv_obj_center(lv_image_);
    lvgl_port_unlock();

    running_ = true;
    xTaskCreate(TaskEntry, "cam_preview", 4096, this, 4, &task_);
    ESP_LOGI(TAG, "预览已启动：%dx%d，左侧黑边 %d，缓冲 %u 字节",
             frame_width_, frame_width_, x_offset_, (unsigned)bytes);
    return true;
}

void CameraPreview::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    /* 等任务自己退出（Task 3 里循环会检查 running_） */
    while (task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (lvgl_port_lock(1000)) {
        if (lv_image_ != nullptr) {
            lv_obj_delete(lv_image_);
            lv_image_ = nullptr;
        }
        if (black_bg_ != nullptr) {
            lv_obj_delete(black_bg_);
            black_bg_ = nullptr;
        }
        lvgl_port_unlock();
    }

    if (swap_buf_ != nullptr) {
        heap_caps_free(swap_buf_);
        swap_buf_ = nullptr;
    }
    esp_camera_deinit();
    ESP_LOGI(TAG, "预览已停止，摄像头已释放");
}

void CameraPreview::TaskEntry(void *arg) {
    static_cast<CameraPreview *>(arg)->Run();
}

void CameraPreview::Run() {
    /* Task 3 里填抓帧循环；本任务先只做生命周期验证 */
    task_ = nullptr;
    vTaskDelete(nullptr);
}
```

- [ ] **Step 3: 在板级代码里接一个【临时】开机自检**

修改 `esp32_s3_cam_board.cc`：把 `panel` 提升为成员，并在构造函数末尾加一次性自检。

```cpp
// 成员区（display_ 旁边）
    esp_lcd_panel_handle_t panel_ = nullptr;
    CameraPreview* preview_ = nullptr;   // 临时：Task 3 会正式接长按
```

`InitializeLcdDisplay()` 里把 `panel` 存进成员（原第 86 行前加一行）：

```cpp
        panel_ = panel;                  /* 存成员：预览模块要用 */
        display_ = new SpiLcdDisplay(panel_io, panel,
```

构造函数末尾（`ESP_LOGI(TAG, "ST7789 ...")` 之后）加：

```cpp
        /* ⚠️ 临时自检（Task 4 会删）：验证摄像头能初始化、能取到帧、能释放 */
        preview_ = new CameraPreview(panel_, DISPLAY_WIDTH, CAMERA_PREVIEW_SIZE);
        if (preview_->Start()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            preview_->Stop();
        } else {
            ESP_LOGE(TAG, "预览自检失败");
        }
```

头部加 `#include "camera_preview.h"`，并在 `config.h` 里加：

```c
/* 预览画面尺寸（正方形）。240 = 垂直正好铺满，水平两侧各留 22px 黑边。 */
#define CAMERA_PREVIEW_SIZE  240
```

- [ ] **Step 4: 构建**

```bash
oesp && cd xiaozhi-esp32 && IDF_PY_BUILD_JOBS=12 idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`
若报 `esp_camera.h: No such file` → `idf_component.yml` 里的 `espressif/esp32-camera` 未被 main 自动依赖，
需在 `main/CMakeLists.txt` 的 `PRIV_REQUIRES` 里补 `espressif__esp32-camera`。

- [ ] **Step 5: 烧写并看串口**

```bash
idf.py -p /dev/ttyACM0 flash
cd .. && oesp && python xiaozhi_monitor.py -t 30
```

Expected（**本任务的验收判据**）：
```
I (xxx) CameraPreview: 预览已启动：240x240，左侧黑边 22，缓冲 115200 字节
I (xxx) CameraPreview: 预览已停止，摄像头已释放
```
且**没有** `esp_camera_init 失败`、没有 `E` 级日志、板子不重启。

- [ ] **Step 6: 提交（需用户确认）**

```bash
cd xiaozhi-esp32
git add main/boards/esp32-s3-cam-ics43434/camera_preview.h \
        main/boards/esp32-s3-cam-ics43434/camera_preview.cc \
        main/boards/esp32-s3-cam-ics43434/esp32_s3_cam_board.cc \
        main/boards/esp32-s3-cam-ics43434/config.h
git commit -m "摄像头预览：摄像头生命周期模块 + 开机临时自检"
```

---

### Task 3: 抓帧循环 + 长按切换

**Files:**
- Modify: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/camera_preview.cc`（填 `Run()`）
- Modify: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/esp32_s3_cam_board.cc`（换成长按切换）

**Interfaces:**
- Consumes: `CameraPreview::Start()/Stop()`（Task 2）、`camera_frame_swap_bytes()`（Task 1）
- Produces: 无新接口

- [ ] **Step 1: 填抓帧循环**

把 `camera_preview.cc` 的 `Run()` 换成：

```cpp
void CameraPreview::Run() {
    const size_t pixels = (size_t)frame_width_ * frame_width_;
    uint32_t frames = 0;

    while (running_) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == nullptr) {
            ESP_LOGW(TAG, "取帧失败");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 字节序交换（必需，见 spec §7.1）。摄像头给的是 240x240 RGB565。 */
        camera_frame_swap_bytes((uint16_t *)swap_buf_, (const uint16_t *)fb->buf, pixels);
        esp_camera_fb_return(fb);

        /* 只在这一小段持锁：让 LVGL 把这块标脏，渲染与 SPI 同步都交给它 */
        if (lvgl_port_lock(100)) {
            lv_obj_invalidate(lv_image_);
            lvgl_port_unlock();
        }

        frames++;
        if ((frames % 100) == 0) {
            ESP_LOGI(TAG, "已推 %u 帧", (unsigned)frames);
        }
        /* 不额外延时：帧率由取帧与 LVGL 渲染自然节流 */
    }

    task_ = nullptr;
    vTaskDelete(nullptr);
}
```

- [ ] **Step 2: 板级换成"长按切换"**

`esp32_s3_cam_board.cc` 的 `InitializeButtons()` 里，`OnClick` **保持原样不动**（回归要求），
在其后追加：

```cpp
        /* 长按 = 切进/切出摄像头预览（短按仍是触发对话，见 config.h 与 spec D3） */
        boot_button_.OnLongPress([this]() {
            if (preview_ == nullptr) {
                return;
            }
            if (preview_->IsRunning()) {
                preview_->Stop();
            } else if (!preview_->Start()) {
                ESP_LOGE(TAG, "进入预览失败");
            }
        });
```

同时把 `OnClick` 改成"预览中短按 = 退出预览"（spec D4，防卡死）—— 在 `OnClick` 回调**最前面**插入：

```cpp
            if (preview_ != nullptr && preview_->IsRunning()) {
                preview_->Stop();
                return;
            }
```

并**删掉 Task 2 的临时自检代码块**（`preview_ = new CameraPreview(...)` 那段），改成在成员初始化处建对象：

```cpp
        preview_ = new CameraPreview(panel_, DISPLAY_WIDTH, CAMERA_PREVIEW_SIZE);
```

- [ ] **Step 3: 构建 + 烧写**

```bash
oesp && cd xiaozhi-esp32 && IDF_PY_BUILD_JOBS=12 idf.py build 2>&1 | tail -3
idf.py -p /dev/ttyACM0 flash
```

Expected: `Project build complete.` + 烧写 `Hash of data verified.`

- [ ] **Step 4: 目视验收（本任务的核心判据）**

1. 开机进 idle，**长按 BOOT 约 1 秒** → 屏幕出现**摄像头画面**，居中，两侧黑边
2. 画面**颜色正常**（若发花/偏色 → 字节序问题，见下方排障）
3. 再**长按** → 回到 xiaozhi UI
4. **短按** BOOT → 仍触发一轮对话（原行为未被破坏）
5. 长按进预览后**短按** → 能退出（不卡死）
6. 串口出现 `CameraPreview: 已推 N 帧`，且帧率合理

排障对照：

| 现象 | 原因 | 动作 |
|---|---|---|
| 画面颜色发花/偏色 | 字节序（本应已处理） | 检查 `camera_frame_swap_bytes` 是否真的被调用 |
| 帧率很低（<5fps） | PSRAM 带宽 | 启用 spec §6 的音频暂停；或降 `FRAMESIZE` |
| 画面上下/左右反 | 传感器镜像 | `sensor->set_hmirror/set_vflip` |
| 进出预览偶发卡死 | LVGL 锁竞态 | 检查 `Stop()` 是否等到 `task_ == nullptr` |
| 退出后屏幕仍是画面 | image 没删掉 | 检查 `Stop()` 里的 `lv_obj_delete` |

- [ ] **Step 5: 提交（需用户确认）**

```bash
cd xiaozhi-esp32
git add main/boards/esp32-s3-cam-ics43434/camera_preview.cc \
        main/boards/esp32-s3-cam-ics43434/esp32_s3_cam_board.cc
git commit -m "摄像头预览：抓帧循环 + 长按 BOOT 切换"
```

---

### Task 4: 收尾 —— 去临时代码 + 文档同步

**Files:**
- Modify: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/README.md`
- Modify: `xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/config.h`
- Modify: `hello_world/docs/plans/2026-10-07-xiaozhi-camera-preview.md`（状态改为"已实现"）

**Interfaces:** 无

- [ ] **Step 1: 确认临时自检已删干净**

```bash
cd xiaozhi-esp32 && grep -n "自检\|临时" main/boards/esp32-s3-cam-ics43434/esp32_s3_cam_board.cc
```

Expected: 无输出（若还有，删掉）

- [ ] **Step 2: 更新板级 README**

在 `README.md` 的「已知限制」前后加一节：

```markdown
## 摄像头实时预览

**长按 BOOT 约 1 秒**在 xiaozhi UI 与摄像头预览之间切换；预览中**短按**也能退出。
画面 **240×240 居中**，两侧各 22px 黑边（屏 284 宽）。目标 ~15fps。

- 上屏走 **LVGL 顶层全屏 `lv_image`**，SPI 完成同步由 LVGL 负责
  —— ⚠️ **不要**改成"绕过 LVGL 直推面板"：完成回调被 esp_lvgl_port 占着（spec R2）
- 帧数据**必须逐像素交换字节序**（`camera_frame_swap_bytes`），依据见 spec §7.1
- 退出时 `esp_camera_deinit()` 会把 230KB 帧缓冲还给 PSRAM —— **别删这一句**
- 摄像头归**板级自管**，不实现 `GetCamera()` → 上游「拍照问 AI」工具不注册
```

- [ ] **Step 3: 更新 spec 状态**

把 `2026-10-07-xiaozhi-camera-preview.md` 顶部状态改为：

```markdown
> 2026-10-07 · 状态：**已实现**（见 `2026-10-07-xiaozhi-camera-preview-impl.md`）
```

- [ ] **Step 4: 最终验收（跑一遍 spec §9 的全部判据）**

```bash
oesp && cd xiaozhi-esp32 && IDF_PY_BUILD_JOBS=12 idf.py build 2>&1 | tail -3
idf.py -p /dev/ttyACM0 flash
cd .. && oesp && python xiaozhi_monitor.py -t 30
```

对照 spec §9 六条逐条确认，并把 `SystemInfo: free sram` 在"预览前 / 预览中 / 退出后"三个时刻的值记下来，
确认退出后**回落**（证明 `deinit` 生效）。

- [ ] **Step 5: 提交（需用户确认，两个仓库分别提交）**

```bash
# ① 先推 submodule
cd xiaozhi-esp32
git add main/boards/esp32-s3-cam-ics43434/README.md main/boards/esp32-s3-cam-ics43434/config.h
git commit -m "摄像头预览：文档同步（README + config.h）"
git push origin s3cam-port
# ② 再提交父仓库（文档 + submodule 指针）
cd ..
git add hello_world/docs/plans/2026-10-07-xiaozhi-camera-preview.md \
        hello_world/docs/plans/2026-10-07-xiaozhi-camera-preview-impl.md \
        xiaozhi-esp32
git commit -m "文档：摄像头实时预览的设计与实现计划"
```
