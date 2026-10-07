# 小智固件：摄像头实时预览到屏幕（设计）

> 2026-10-07 · 状态：**已实现并验收通过**（实现计划：`2026-10-07-xiaozhi-camera-preview-impl.md`）
> ⚠️ 实测更正：板上摄像头是 **OV5640**（`Detected OV5640 camera`），非 OV2640；引脚兼容，映射不变
> 关联：`2026-10-04-xiaozhi-s3cam-port.md`（移植总计划）
> 代码位置：`xiaozhi-esp32/main/boards/esp32-s3-cam-ics43434/`（submodule）

## 0. 修订记录（两条，都推翻了初版判断）

| # | 初版写的 | 实际 | 影响 |
|---|---|---|---|
| R1 | "上游没有任何板子把摄像头画面显示到屏幕上" | ❌ **错**。上游 `Esp32Camera::Capture()` 会调 `LcdDisplay::SetPreviewImage()` 把帧送上屏（`mcp_server.cc:115` 的 `take_photo` 工具触发） | 本功能仍是新的（上游是**聊天区小气泡**、一次性），但**字节序和同步有现成通路可复用** |
| R2 | 方案 A："绕过 LVGL，直接 `esp_lcd_panel_draw_bitmap` 推帧" | ❌ **有隐藏硬伤**：判断 DMA 完成只能靠 `on_color_trans_done`，而**该回调已被 esp_lvgl_port 占用**（`esp_lvgl_port_disp.c:124`）；抢过来就永久搞坏 LVGL（`esp_lcd` 无 getter，无法还原）。退而靠 SPI 队列阻塞（`trans_queue_depth=10`）→ **预览延迟约 500ms** | **改为经 LVGL**（见 §3） |

## 1. 目标

长按 BOOT 键在「xiaozhi UI」与「摄像头实时预览」之间切换。
预览为**全屏 240×240 居中 + 两侧黑边**，目标帧率 **~15fps**（见 §5 的修正）。

## 2. 已确认的需求决策

| # | 决策 | 依据 |
|---|---|---|
| D1 | 全屏预览，**按键切换** | 用户选定 |
| D2 | **240×240 + 两侧黑边** | 用户选定（不裁剪不缩放，实现最稳） |
| D3 | **长按 BOOT（~1s）** 切换预览；**短按仍是「触发对话」** | 用户选定（BOOT 短按已被占用，见 CLAUDE.md 实测记录） |
| D4 | **预览中短按也退出** | 防止卡在预览里出不来 |
| D5 | **经 LVGL 上屏**（顶层全屏 `lv_image`），不绕过 LVGL | 见 §3 修正 |

## 3. 方案选型

### 3.1 摄像头归谁管：板级自管（选定）

上游 `Esp32Camera` 的接口（`Capture()` / `Explain()` / `SetExplainUrl()`）
**不暴露原始 RGB565 帧**，所以本地预览拿不到它的帧缓冲。

| | **A. 板级自管（✅ 选定）** | B. 扩展上游 `Esp32Camera` |
|---|---|---|
| 上游文件改动 | **零**（只加板级文件） | 改 `main/boards/common/esp32_camera.{h,cc}` |
| 合并上游冲突 | 无 | 有风险 |
| 并发复杂度 | 低 | 高（JPEG 编码线程与预览抢帧） |

⚠️ **代价**：将来做「拍照问 AI」时需重构一次。本方案**不实现 `GetCamera()`**
（基类返回 `nullptr` → 云端识图工具不注册 → 无冲突）。

### 3.2 上屏路径：经 LVGL（修正后选定，见 R2）

```
预览任务（自己抓帧、换字节序）
      ↓ 把帧数据交给 LVGL 的 lv_image 对象
LVGL 渲染任务（本来就跑着）
      ↓ 它自己的 on_color_trans_done 回调做 SPI 同步   ← 关键：不用我们操心
ST7789
```

**为什么这样最好**：SPI 完成同步是 LVGL 已经做好的事；我们绕开它反而要跟它抢回调。
**且不需要 `lvgl_port_stop/resume`** —— 少一层模式切换和竞态。

⚠️ 与上游的关系：上游是**聊天区 70%×50% 的小气泡、一次性**（`lcd_display.cc:700`）；
本功能是**全屏、连续、按键切换**的新功能，只借用它已经踩平的**字节序通路**。

## 4. 数据通路

```
[长按 BOOT ~1s]
  ├─ 进入预览
  │   1. esp_camera_init(RGB565 / 240×240 / PSRAM / fb_count=2)
  │   2. 在 LVGL 顶层建一个全屏 lv_image（覆盖 UI）
  │   3. 建预览任务：
  │        loop:  fb = esp_camera_fb_get()
  │               逐像素 __builtin_bswap16 到自己的缓冲   ← 字节序，见 §7.1
  │               取 LVGL 锁 → 更新 image 的数据指针 + 标脏 → 放锁
  │               esp_camera_fb_return(fb)
  └─ 退出预览
      1. 停预览任务（并等待其真正退出）
      2. 删除那个 lv_image 对象        ← UI 自然露出，**不需要手动重绘**
      3. esp_camera_deinit()           ← 释放 230KB 帧缓冲
```

## 5. 关键参数

| 项 | 值 | 理由 |
|---|---|---|
| `frame_size` | `FRAMESIZE_240X240` | 垂直正好 240；水平居中留 **22px** 黑边（(284−240)/2） |
| `pixel_format` | `PIXFORMAT_RGB565` | 免解码 |
| `fb_count` | **2** | 双缓冲 |
| `fb_location` | `CAMERA_FB_IN_PSRAM` | 240×240×2 = **115KB/帧**，放不进内部 SRAM |
| `grab_mode` | `CAMERA_GRAB_WHEN_EMPTY` | 跟随上游参考板 |
| `xclk_freq_hz` | 20 MHz（`XCLK_FREQ_HZ`，config.h 已有） | 摄像头引脚已由原理图实证 |
| 目标帧率 | **~15fps**（原写 20） | 经 LVGL 多一次拷贝；SPI 满屏一帧约 27ms 是硬上限 |

## 6. 并发与资源

| 决策 | 理由 |
|---|---|
| **不用 `lvgl_port_stop/resume`** | 经 LVGL 后不需要；少一层竞态（修正后简化） |
| 每帧只在**更新 image 指针时**短暂持有 LVGL 锁 | 用 `DisplayLockGuard`（`LcdDisplay` 已有）或 `lvgl_port_lock/unlock` |
| 退出时**必须** `esp_camera_deinit()` | 否则 230KB 帧缓冲常占 PSRAM，与 LVGL 的 2MB 图像缓存抢 |
| **音频暂停改为「条件触发」** | 先**不停**音频，实测帧率不够再停（YAGNI，避免为未验证的假设引入复杂度） |

⚠️ **若实测需要暂停音频**：优先停 `AudioService`；若不支持运行时停启，
退而只关 I2S 通道（`codec->EnableInput/Output(false)`）。判据：**无音频相关 `E` 级日志 + 帧率达标**。

## 7. 风险与对策

| 风险 | 判据（怎么知道是它） | 对策 |
|---|---|---|
| PSRAM 带宽不足 | 帧率明显低于 15fps | 降分辨率/帧率；或启用 §6 的音频暂停 |
| 退出后 UI 没恢复 | 退出后屏幕还是摄像头画面 | 删 image 对象后显式触发一次重绘（兜底） |
| 摄像头 SCCB(I²C) 与现有 I²C 冲突 | `esp_camera_init` 失败 | 让 esp32-camera **自建** I²C（不指定 `sccb_i2c_port`） |
| 画面上下/左右反 | 画面内容镜像 | `sensor->set_hmirror/set_vflip`（本次先不做，出问题再说） |
| LVGL 锁与预览任务竞态 | 进出预览偶发卡死 | 预览任务只在更新指针时持锁；退出时先停任务再删对象 |

### 7.1 字节序为什么是"必需"（证据）

上游 `main/boards/common/esp32_camera.cc`：

```cpp
bool swap_bytes_enabled_ = true;   // esp32_camera.h:26 —— 默认就开
...
    if (swap_bytes_enabled_) {
        for (size_t i = 0; i < pixel_count; i++) {
            dst[i] = __builtin_bswap16(src[i]);      // :102-105
        }
    }
```

**上游对每一帧都做逐像素交换且默认开启** —— 这是"传感器（本板实装 OV5640）输出的 RGB565 字节序
与显示屏期望的不一致"的直接证据。所以本实现**从一开始就做交换**，不留作"出问题再说"。

⚠️ 成本：240×240 = 57600 次/帧，约 1~2ms，相对一帧 27ms 的 SPI 传输可接受。

## 8. 改动清单

| 文件 | 改动 |
|---|---|
| `.../esp32-s3-cam-ics43434/camera_frame.{h,c}` | **新增** —— 纯 C 逻辑（字节序交换、居中窗口计算），**可在 PC 上 gcc 单测** |
| `.../esp32-s3-cam-ics43434/camera_preview.{h,cc}` | **新增** —— 摄像头生命周期 + 抓帧任务 + LVGL image 更新 |
| `.../esp32-s3-cam-ics43434/test/host/test_camera_frame.c` | **新增** —— 主机侧单测（放在子目录，**不会被 `file(GLOB .../*.c)` 收进固件**） |
| `.../esp32-s3-cam-ics43434/esp32_s3_cam_board.cc` | 存 `panel_`/`panel_io_` 为成员；接 `boot_button_.OnLongPress()`；进出预览 |
| `.../esp32-s3-cam-ics43434/config.h` | 补预览参数（预览宽高、黑边） |
| **上游共享文件** | **不动** |

## 9. 验收判据

1. 长按 BOOT → 屏幕出现摄像头画面（居中 240×240，两侧黑边）
2. 再长按（或短按）→ 回到 xiaozhi UI，**唤醒词/对话功能仍正常**
3. 预览期间**不崩、不重启**，日志无 `E` 级错误
4. 退出后 **PSRAM 回落**（对比 `SystemInfo: free sram`，证明 `deinit` 生效）
5. **短按 BOOT 仍触发对话**（原行为未被破坏 —— 回归检查）
6. 主机侧单测 `test_camera_frame` 全绿

## 10. 后续（本次明确不做）

- 「拍照问 AI」云端识图（需扩展 `Esp32Camera` 或重构预览模块）
- 分辨率/帧率调优、画面镜像、拍照存 SD 卡
