# 唤醒与聆听结束：xiaozhi-esp32 的实现研究

**日期**：2026-09-29
**起因**：用户参考 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，
想了解 ESP32 上的唤醒与聆听结束是怎么做的，以备后续使用。

**一句话结论**：**xiaozhi 的架构在我们这块板子上基本不可移植** ——
它的核心（AFE + WakeNet9）都依赖 ESP32-S3/P4，**经典 ESP32 不在支持范围内**。
但它有一个**与芯片无关的想法非常有价值**，见 §4。

---

## 1. xiaozhi 是怎么做的

它用乐鑫官方的 **ESP-SR** 框架，核心是 **AFE（声学前端）+ WakeNet（唤醒）+ VAD**。

### 1.1 音频管线

```
麦克风 → I2S 读取 → AFE.feed() → [AEC → NS → BSS → VAD → WakeNet] → AFE.fetch()
                                                                        │
                                        处理后的音频 + VAD 状态 + 唤醒状态 ┘
```

- **帧长 32 ms**（16 kHz 下 512 样本）
- 输入要求 **16 kHz / 16 bit / 双声道**（麦克风 + 参考信号，供 AEC 用）
- 输出单声道
- 唤醒后可用 `disable_wakenet()` 进入 Bypass 模式省 CPU，需要时再 `enable_wakenet()`
- 内存优先分配 PSRAM（`AFE_MEMORY_ALLOC_MORE_PSRAM`）

### 1.2 唤醒的判定

`AFE.fetch()` 返回的 `wakeup_state == WAKENET_DETECTED` 时触发回调，
并用 `res->wakenet_model_index` 区分是**哪个**唤醒词。

### 1.3 聆听结束的判定

**完全靠 AFE 的 VAD**，靠状态转换驱动应用逻辑：

| AFE 状态 | 应用动作 |
|---|---|
| `AFE_VAD_SPEECH` | **开始录音** |
| `AFE_VAD_SILENCE` | **结束录音**（即"聆听结束"） |

VAD 通过 `vad_state_change_callback_` 通知应用层，同时联动 LED 与设备状态。

**VAD 模式是可调的**：`VAD_MODE_0..3+`，**模式越高门限越高、越不易误触发**，
适合嘈杂环境；低模式更灵敏，适合安静环境。项目选的是 `VAD_MODE_3`。

---

## 2. 但我们的芯片不支持

**我们的板子是 ESP32-PICO-V3-02 = 经典 ESP32（Xtensa LX6）**，不是 S3/P4。

| 能力 | xiaozhi 用的 | 经典 ESP32（我们） |
|---|---|---|
| 唤醒词引擎 | **WakeNet9** | ⚠️ **只有 WakeNet5 / 5X2 / 5X3**（旧模型） |
| **AFE 声学前端** | ✅ | ❌ **不支持** |
| **VAD** | AFE 的组成部分 | ❌ **随 AFE，我们没有** |
| 降噪 | NSNet2（神经网络） | ⚠️ 仅传统 WebRTC NS（27 KB RAM，10 ms 帧） |
| 命令词 | MultiNet（mn5q8/mn6/mn7） | ⚠️ 仅 mn2_cn 旧模型 |

**官方原话**（esp-sr issue #107）：

> "ESP32 does not support all algorithm. I recommend to use ESP32-S3"

**且官方文档明确：「新算法将不再支持 ESP32 芯片」。**

### 2.1 唤醒词即便能用，也很受限

经典 ESP32 的 WakeNet5 系列**只有两个官方唤醒词**：

| 唤醒词 | 模型名 |
|---|---|
| Hi, 乐鑫 | `wn5_hilexin` / `wn5_hilexinX3` |
| 你好小智 | `wn5_nihaoxiaozhi` / `wn5_nihaoxiaozhiX3` |

资源占用（WakeNet5 / 5X2 / 5X3）：

| 模型 | 内部 RAM | 整体 | 处理耗时 |
|---|---|---|---|
| WakeNet5 | 15 KB | ~40 KB | 5.5 ms / 30 ms 帧 |
| WakeNet5X2 | 20 KB | ~80 KB | 10.5 ms / 30 ms 帧 |
| WakeNet5X3 | 24 KB | ~120 KB | 18 ms / 30 ms 帧 |

**内存我们够**（520 KB SRAM + 2 MB PSRAM）。但——

- 想要别的唤醒词（比如"开始记录"），必须走**乐鑫付费定制**：
  需 2 万条以上语料、2~3 周训练、按量收费
- 且这是**技术死路**：官方已停止为经典 ESP32 开发新算法

→ **结论：唤醒词在我们的板子上"技术上可行，实用上不划算"。**

---

## 3. 这对我们意味着什么

**我们的 PRD 决策被反向印证了。** 当初把"板端识别"和"唤醒词"列为非目标
（`prd.md` §1.3、`decisions.md` D12.2），当时的理由是"简化范围"；
**现在有了更硬的理由：这块芯片做不到。**

| 我们的设计 | xiaozhi | 评价 |
|---|---|---|
| 按键触发 | 唤醒词 | 我们用按键是**被迫也是合理**的选择 |
| PC 侧 RMS 静音检测 | 板端 AFE VAD | 我们**没有别的选择** —— AFE VAD 在经典 ESP32 上不存在 |
| 板子只采音、不做识别 | 板端做 AFE+唤醒 | 架构不同源于芯片能力不同 |

**VAD 状态机的形态（SPEECH→录、SILENCE→停）我们是照搬的** ——
`prd.md` FR-5 的"静音 5 秒自动结束"就是同一个模式，只是实现从 AFE VAD
换成了 PC 侧 RMS。**这条路是对的，值得保留。**

---

## 4. ★ 唯一真正值得借鉴的：唤醒前 2 秒的预录缓冲

**这是本次研究最有价值的发现，而且与芯片无关。**

xiaozhi 在检测到唤醒词的瞬间，会把**唤醒点之前约 2 秒的音频**从环形缓冲里
取出来（保留 2000/32 ≈ 62 帧），一起编码上传给服务器做校验。

**为什么这对我们特别有价值**：我们的触发是**按键**，而人说话和按按键之间
**必然有延迟** —— 用户往往是**先开口、再伸手按**。结果就是**每句话的开头
都被吃掉**。这不是 bug，是按键触发方式的固有缺陷。

**解决方案**：在固件里维护一个 2 秒的环形缓冲，按下开始录音时，
**先把缓冲里的 2 秒预录音频发出去**，再接实时数据。

成本极低：

| 项 | 值 |
|---|---|
| 内存 | 2 秒 × 16 kHz × 2 字节 = **64 KB**（2 MB PSRAM，占 3%） |
| 实现 | 环形缓冲 + 录音开始时先冲一遍 |
| 收益 | **每句话的开头不再丢失** |

**建议：值得做，且成本远低于唤醒词。** 可作为一个独立的增量任务，
放在阶段 6（按键）时一并实现 —— 因为正是"按键触发"这个方式让它变得必要。

---

## 5. 其他可借鉴的细节

| 细节 | xiaozhi | 对我们 |
|---|---|---|
| 帧长 32 ms / 512 样本 | AFE 的要求 | 我们用 256 样本（16 ms）。**无需改**，这是 AFE 的约束不是通用的 |
| VAD 模式作为门限旋钮 | `VAD_MODE_0..3+` | 我们的 RMS 阈值是同一概念。**可借鉴"提供几档预设"**，而不是只给一个裸阈值 |
| 唤醒后 Bypass 省 CPU | `disable_wakenet()` | 不适用（我们没有唤醒词） |
| Opus 压缩后上传 | 16 kHz 单声道 20 ms 帧 | 不适用 —— 我们串口有 2.8 倍余量，不需要压缩 |
| 唤醒音频送服务器二次校验 | 防误唤醒 | 不适用（我们全本地） |
| 处理任务绑 Core 1 | 保证实时性 | 可留意，但我们的负载远低于它 |

---

## 6. 如果将来真要做唤醒词

唯一现实的路是**换芯片**（ESP32-S3），那时：

- WakeNet9 可用，唤醒词可切换（`set_wakenet()`）
- AFE 全套（AEC/NS/BSS/VAD）可用
- 自定义唤醒词仍可选：付费定制，或**用 TTS 样本自行训练**
  （`_tts` 后缀的模型就是这么来的）

**但注意**：换芯片意味着重做阶段 1 的全部硬件工作（引脚、I2S 配置、
**64 SCK 那个坑**），以及重新验证。**这不是一个小改动，应该作为一个
独立项目来评估，而不是在 voice_notes 里顺手做。**

---

## 参考来源

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
- [xiaozhi 音频处理管线（DeepWiki）](https://deepwiki.com/78/xiaozhi-esp32/3.3-audio-processing-pipeline)
- [xiaozhi 唤醒词检测系统（DeepWiki）](https://deepwiki.com/78/xiaozhi-esp32/3.4-wake-word-detection-systems)
- [ESP-SR 硬件支持矩阵（DeepWiki）](https://deepwiki.com/espressif/esp-sr/1.2-hardware-support-matrix)
- [ESP-SR 可用唤醒词模型（DeepWiki）](https://deepwiki.com/espressif/esp-sr/3.1-available-wake-word-models)
- [ESP32 经典 WakeNet 唤醒词模型文档](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32/wake_word_engine/README.html)
- [ESP-SR AFE 声学前端文档（S3）](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/audio_front_end/)
- [esp-sr issue #107：ESP32 不支持所有算法](https://github.com/espressif/esp-sr/issues/107)
- [esp-sr 组件说明](https://components.espressif.com/components/espressif/esp-sr/versions/1.8.0/readme)
- [Espressif ESP-SR 介绍（cnx-software）](https://www.cnx-software.com/2023/07/17/espressif-esp-sr-enables-on-device-speech-recognition-framework-on-esp32-s3-and-esp32-wisocs/)
