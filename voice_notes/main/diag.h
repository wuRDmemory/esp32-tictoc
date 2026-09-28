/*
 * 诊断命令集。
 *
 * ⚠️ **不要删这些命令。** 阶段 1 的排查几乎全靠它们：
 *    raw   —— 判定位对齐（看哪个字节恒为 0）
 *    level —— 验证麦克风是否响应声音（拍手能否让电平跳 20dB）
 *    rec   —— 双声道判读，区分「没数据」和「选错声道」
 *    还有 shift / chan / dc 三个不改硬件就能排除软件因素的开关
 *
 * 删掉等于把下次排查的工具扔掉。
 */
#pragma once

void diag_print_cfg(void);
void diag_print_help(void);

/* 处理一行诊断命令（不含换行）。未知命令会打印提示。 */
void diag_handle_command(const char *line);
