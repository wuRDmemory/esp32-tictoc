/*
 * ICS-43434 采音。
 *
 * ⚠️ 本文件的所有常量都由 docs/hardware.md §7 的**实测**确定，不要重新推导：
 *    - 必须 STEREO + 32bit 才能凑出 ICS-43434 要求的 64 SCK/帧
 *      （直觉上的 MONO 会得到 32 SCK/帧，麦克风直接不工作）
 *    - dma_frame_num=256 是上限 511 之下的安全值
 *    - 24bit 数据在 32bit 字的 [31:8]，故 >>16 得 16bit
 *    - 本模块有效分辨率约 19 位（国产克隆模块特征，不是 bug）
 *    - 输出带 364~424 的直流偏置且会漂移，必须做直流阻断
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "board.h"      /* 引脚 / 时钟源 / 采样率都来自板级定义 */

#define I2S_MIC_SAMPLE_RATE     BOARD_SAMPLE_RATE

/* 引脚号转发一下，供 diag 打印配置用。
 * ⚠️ 真正的定义在 board.h —— 这里只是别名，别在这儿改数值。 */
#define I2S_MIC_PIN_BCLK        BOARD_I2S_BCLK
#define I2S_MIC_PIN_WS          BOARD_I2S_WS
#define I2S_MIC_PIN_DIN         BOARD_I2S_DIN

/* 每次读 256 帧。上限推导：dma_buffer_size = frames × slots × slot_bits/8 ≤ 4092
 * → 256 × 2 × 32/8 = 2048 字节 ✓（若取 512 则 4096 > 4092，会失败） */
#define I2S_MIC_FRAMES_PER_READ BOARD_FRAMES_PER_READ

/* 初始化。内部已做 APLL → 默认时钟源的回退，实际用哪个用
 * i2s_mic_clk_name() 查询（别让"以为用了 APLL"成为幻觉） */
esp_err_t i2s_mic_init(void);

const char *i2s_mic_clk_name(void);

/* 读一块，返回样本数；<0 表示错误。
 * 输出为**当前选中声道**的 16bit 样本，已过直流阻断。 */
int i2s_mic_read(int16_t *out, int max_samples);

/* 读一块，两路都给出（均过直流阻断）。供 `rec` 命令做双声道判读用 ——
 * 只有同时看到两路，才能区分「完全没数据」和「数据在另一个声道」。 */
int i2s_mic_read_both(int16_t *left, int16_t *right, int max_samples);

/* 读一块，输出**未经位移**的原始 32bit 字。供 `raw` 诊断命令判断位对齐用。
 * 注意：本函数不推进直流阻断状态（它是诊断用的旁路）。 */
int i2s_mic_read_raw(int32_t *left, int32_t *right, int max_frames);

void i2s_mic_set_shift(int n);
int  i2s_mic_get_shift(void);

void i2s_mic_set_chan(int n);
int  i2s_mic_get_chan(void);

void i2s_mic_set_dc_block(bool on);
bool i2s_mic_get_dc_block(void);

/* 重置直流阻断状态。每次开始录音前调用，避免上一段的残留状态污染开头 */
void i2s_mic_reset_dc(void);

/* 丢弃前 125ms。直流阻断器收敛需约 1000 个样本，不丢的话起始瞬态会变成
 * 一个 400+ 的假峰值（实测踩过：静音采集报出峰峰 589，其中 470 是瞬态）。 */
void i2s_mic_warmup(void);

/* 滤波前的原始均值，供 `rec` 命令报告"直流有多大" */
void   i2s_mic_raw_reset(void);
double i2s_mic_raw_dc(void);
