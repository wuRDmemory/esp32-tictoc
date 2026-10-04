/*
 * AFE 的 ESP-SR 实现 —— ESP32-S3 的板端 VAD 判停（decisions.md D13）。
 *
 * 目标：把「用户说完话 → 系统知道」的延迟，从 **PC 侧 RMS 的静音 5 秒**
 *       降到**亚秒级**。这是用户「完全参考 xiaozhiAI 就行，他的反应挺快的」
 *       这句话的直接落点。
 *
 * ⚠️ **任务模型（本文件最重要的约束，实测改过一次）**：
 *
 *    `fetch()` 默认**阻塞 2000ms**，绝不能放进 16ms 的主循环 —— 那是 2 秒的音频黑洞。
 *
 *    最初试过"主循环里用 `fetch_with_delay(afe, 0)` 非阻塞轮询"，**上板证伪了**：
 *    AFE 是 32ms 节拍的流水线（还带处理延迟），用 16ms 去轮询它，**每次空取都会打印**
 *      `W AFE: Ringbuffer of AFE is empty, Please use feed() to write data`
 *    → 刷屏到把真正的日志全淹掉。
 *
 *    **现在用独立任务阻塞 fetch**（也是官方例子的用法）：
 *      - feed 仍由主循环做（写环形缓冲，本身非阻塞）
 *      - fetch 在一个独立任务里**阻塞等待**，不空转、不刷屏
 *      - 任务只更新两个**单调递增的计数器**，主循环侧做边沿检测
 *        → 无锁、不丢事件（单字读写天然原子，见 afe_poll_event 的说明）
 *
 * ⚠️ **音频路径不受影响**：本实现只读 `vad_state`，不把 AFE 处理后的音频交出去
 *    （afe.h 也没有这个接口，是故意的）。发给 PC 的始终是原始 PCM（D13.4）。
 *
 * ⚠️ **`vad_cache` 我们不用**：issue #168 说"不处理会丢第一个字"，但那是针对
 *    **"用 VAD 触发开始录音"** 的场景。我们是按键/命令触发，VAD **只判停**，
 *    判停时那 1 秒静音早就实时发给 PC 了。详见 docs/plans/2026-10-04-d13-afe-vad.md §2.3。
 *
 * 版本注意：本实现针对 **esp-sr 2.5.5 的 v2 API**。v1 的 `AFE_CONFIG_DEFAULT()`
 * 宏和 `ESP_AFE_SR_HANDLE` 全局句柄在 v2 已删除 —— 网上大量教程是 v1 的。
 */
#include "afe.h"

#include "board.h"

#if BOARD_HAS_ESPSR

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_vad.h"

static const char *TAG = "afe_espsr";

/* ---- 可调参数：全部集中在顶部，方便用 `vad` 命令标定（prd.md Q9）---- */

/* 静音多久算「说完了」。**这是 Q9 要标定的核心参数。**
 * 太大 → 用户还是要干等，前移失去意义；太小 → 句子里换气处会被切断。 */
#define AFE_VAD_MIN_NOISE_MS   1000

/* VAD 模式：0=Normal … 4=Very Very Very Aggressive。
 *
 * ⚠️ **两处参考值不一致，不要凭直觉调，必须用 `vad` 命令实测**：
 *    - 官方 voice_activity_detection 例子用 VAD_MODE_1
 *    - xiaozhi 用 VAD_MODE_3（docs/wakeword-research.md §1.3）
 *    - ⚠️ "aggressive" 在 WebRTC VAD 的语义里是**"更激进地滤除非语音"**，
 *      即**更容易判成静音** —— 若是这个语义，VAD_MODE_3 反而更容易切断句子。
 *      **方向未确认**，所以先用官方例子的值。 */
#define AFE_VAD_MODE           VAD_MODE_1

/* 语音要持续这么久才翻到 SPEECH。官方默认 128，库内有警告说超过 512 会引入无谓延迟 */
#define AFE_VAD_MIN_SPEECH_MS  128

/* 每通道样本数上限：32ms@16k = 512，留一倍余量给将来的帧长变化 */
#define AFE_MAX_CHUNK   1024
/* 最多按 2 通道喂 —— "M" 万一不行要退到 "MN"（第二路填 0），见下方 feed() */
#define AFE_MAX_NCH     2

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t        *s_data;

static int s_chunk;      /* 每通道样本数，**运行时查询** */
static int s_nch;        /* 通道数，"M" → 1 */
static int s_fill;       /* 单声道攒帧进度（样本数） */

static int16_t s_mono[AFE_MAX_CHUNK];              /* 攒起来的单声道样本 */
static int16_t s_frame[AFE_MAX_CHUNK * AFE_MAX_NCH]; /* 交给 feed() 的（可能交织的）数据 */

/* ---- 跨任务共享：只更新三个字，靠"单调计数器"避免加锁 ----
 *
 * 为什么不用事件队列：计数器只在**任务侧自增**、**主循环侧读取**，
 * 单字读写天然原子，所以既不需要临界区，也不会丢事件。
 * （丢事件在这里是真问题：漏掉一次 SPEECH_END，判停就永远不会触发。） */
static volatile uint32_t s_end_cnt;     /* SPEECH → SILENCE 的次数 */
static volatile uint32_t s_start_cnt;   /* SILENCE → SPEECH 的次数 */
static volatile bool     s_speech;      /* 当前状态，供 `vad` 命令显示 */

static uint32_t s_seen_end;             /* 主循环侧：已消费到哪 */
static uint32_t s_seen_start;

/* 任务侧私有的基准状态。reset 时置 false 让它重新建立基准 ——
 * 否则上一段录音结束时的残留状态会被算成一次跳变。 */
static volatile bool     s_task_have;
static bool              s_task_last;
static volatile uint32_t s_empty_fetch;   /* fetch 取空的累计次数（诊断用） */

#define AFE_TASK_STACK   6144
#define AFE_TASK_PRIO    5

/* ------------------------------------------------------------------ */
/* fetch 任务：**阻塞**等待 AFE 产出，做状态边沿检测。
 *
 * ⚠️ 必须用阻塞版 `fetch()` 而不是 `fetch_with_delay(…, 0)`：
 *    后者在无数据时会打印 `W AFE: Ringbuffer of AFE is empty…`，
 *    以 16ms 的节奏轮询 32ms 节拍的流水线 = 刷屏。这是实测踩过的。 */
static void afe_fetch_task(void *arg)
{
    (void)arg;

    while (1) {
        afe_fetch_result_t *res = s_afe->fetch(s_data);

        if (res == NULL || res->ret_value == ESP_FAIL) {
            /* ⚠️ **取空在"按需录音"架构下是正常现象，不是故障。**
             *
             * 官方例子是 feed 任务**永不停歇**地喂，所以 fetch 永远有数据；
             * 我们是"录了才有音频"，且 `afe_feed` 由主循环按需调用 ——
             * 只要喂入节奏被打断（例如 `vad` 命令在打印，而打印是慢操作），
             * AFE 输出缓冲就会被取空。
             *
             * 所以这里**降级为 DEBUG**：不刷屏、但也不丢信息 ——
             * 累计次数会每 200 次汇总成一条 WARN 报出来（见下）。
             *
             * ⚠️ 出错**不能退出任务** —— 退出会让判停永久失效且无人察觉。 */
            if (++s_empty_fetch % 200 == 1) {
                ESP_LOGW(TAG, "fetch 取空累计 %u 次（按需录音下属正常；"
                              "持续快速增长才说明喂入节奏有问题）",
                         (unsigned)s_empty_fetch);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const bool speech = (res->vad_state == VAD_SPEECH);
        s_speech = speech;

        if (!s_task_have) {
            /* 第一帧只建立基准，不算跳变 ——
             * 否则开机时若初始态是 SPEECH，会凭空记一次 SPEECH_END，
             * 而 SPEECH_END 会让录音**立刻停止**。 */
            s_task_have = true;
            s_task_last = speech;
            continue;
        }
        if (speech == s_task_last) continue;

        s_task_last = speech;
        if (speech) s_start_cnt++;
        else        s_end_cnt++;
    }
}

/* ------------------------------------------------------------------ */

const char *afe_name(void) { return "esp-sr"; }

bool afe_available(void) { return s_data != NULL; }

esp_err_t afe_init(void)
{
    if (s_data) return ESP_OK;              /* 幂等 */

    /* 把 AFE 库的日志级别提到 ERROR。
     *
     * 理由：库在 fetch 撞上空缓冲时会打 `W Ringbuffer of AFE is empty`。
     * 官方例子遇不到（它的 feed 任务永不停歇），而我们是**按需录音** ——
     * 喂入节奏一被打断就触发，把真正的日志全淹掉（实测：`vad` 12 秒刷 21 条）。
     *
     * ⚠️ **这不是"把警告藏起来"**：取空次数我们在任务里自己统计、每 200 次汇总
     *    上报一条（见 s_empty_fetch），信息没丢，只是不再刷屏。E 级仍然可见。
     *    另一个副作用是好的：console 与音频帧**共用同一条通路**，
     *    少打日志就少一分污染音频流的风险。 */
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    /* 单麦、无参考 → "M"。
     *
     * 依据：v1 文档明文 "if AEC is not required… users can only configure one
     * channel of mic data, and the ref_num can be set to 0"；v2 二进制里
     * input_format 唯一的失败路径是"找不到 M 通道"。
     * ⚠️ 但**官方 v2 没有任何例子用 "M"** —— 这是本次最大的未知。
     *    万一失败，退路是把 "M" 改成 "MN"（喂 2 通道，第二路填 0），
     *    下面 afe_feed() 已支持。**不要**改成 "MR" 填 0 —— 那会让 AEC 白跑。 */
    afe_config_t *cfg = afe_config_init("M", NULL, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init 失败 —— input_format=\"M\" 可能不被接受");
        return ESP_FAIL;
    }

    /* 只要 VAD，其余全关：
     *   AEC / SE —— 需要参考信号或多通道，我们单麦没有（库本来也会自动关，显式写清意图）
     *   NS       —— 库自己警告 "Noise Suppression may reduce the accuracy of
     *               speech recognition. It is not recommended to turn it on."
     *   WakeNet  —— 唤醒词，D12.2 待重审，本次不引入
     *   AGC      —— 不需要 */
    cfg->aec_init     = false;
    cfg->se_init      = false;
    cfg->ns_init      = false;
    cfg->wakenet_init = false;
    cfg->agc_init     = false;

    cfg->vad_init          = true;
    cfg->vad_mode          = AFE_VAD_MODE;
    cfg->vad_min_noise_ms  = AFE_VAD_MIN_NOISE_MS;
    cfg->vad_min_speech_ms = AFE_VAD_MIN_SPEECH_MS;
    /* memory_alloc_mode 不显式设 —— 让库按 Kconfig/默认决定。
     * S3 上 8MB PSRAM 充裕，internal RAM 更宝贵。 */

    /* 官方排错第一步：打印真实生效值。
     * 用来确认 input_format / pcm_config / aec_init 确实是我们要的，
     * 而不是"以为配了"（本项目在 sdkconfig 上踩过静默失效的坑）。 */
    ESP_LOGI(TAG, "AFE 配置（请核对 input_format 与 aec_init）:");
    afe_config_print(cfg);

    s_afe = esp_afe_handle_from_config(cfg);
    if (s_afe == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config 失败");
        afe_config_free(cfg);
        return ESP_FAIL;
    }

    s_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (s_data == NULL) {
        ESP_LOGE(TAG, "create_from_config 失败（内存不足？PSRAM 没开？）");
        return ESP_FAIL;
    }

    /* ⚠️ 帧长必须运行时查询，不能写死：feed() 的签名里**没有长度参数**，
     *    喂错长度会越界读、污染下一帧，而且**不报错**。 */
    s_chunk = s_afe->get_feed_chunksize(s_data);
    s_nch   = s_afe->get_feed_channel_num(s_data);
    ESP_LOGI(TAG, "feed: 每通道 %d 样本 × %d 通道（本层每次攒 %d 个单声道样本）",
             s_chunk, s_nch, s_chunk);

    if (s_chunk <= 0 || s_nch <= 0 || s_nch > AFE_MAX_NCH || s_chunk > AFE_MAX_CHUNK) {
        ESP_LOGE(TAG, "帧长/通道数超出本层缓冲（chunk≤%d, nch≤%d）—— 请调大 AFE_MAX_*",
                 AFE_MAX_CHUNK, AFE_MAX_NCH);
        s_afe->destroy(s_data);
        s_data = NULL;
        return ESP_FAIL;
    }

    if (xTaskCreate(afe_fetch_task, "afe_fetch", AFE_TASK_STACK, NULL,
                    AFE_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建 fetch 任务失败");
        s_afe->destroy(s_data);
        s_data = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "fetch 任务已启动（阻塞等待，不轮询）");
    return ESP_OK;
}

void afe_feed(const int16_t *pcm, int n)
{
    if (s_data == NULL || pcm == NULL || n <= 0) return;

    while (n > 0) {
        const int space = s_chunk - s_fill;
        const int take  = (n < space) ? n : space;

        memcpy(&s_mono[s_fill], pcm, (size_t)take * sizeof(int16_t));
        s_fill += take;
        pcm    += take;
        n      -= take;

        if (s_fill < s_chunk) continue;     /* 还没攒够一帧 */

        /* ---- 攒够一帧，按通道数展开后喂给 AFE ---- */
        if (s_nch == 1) {
            s_afe->feed(s_data, s_mono);
        } else {
            /* 多通道：第 0 路是麦克风，其余填 0。
             * 这是 "M" 不被接受时的退路（见 afe_init 的说明）。 */
            for (int i = 0; i < s_chunk; i++) {
                s_frame[i * s_nch] = s_mono[i];
                for (int c = 1; c < s_nch; c++) s_frame[i * s_nch + c] = 0;
            }
            s_afe->feed(s_data, s_frame);
        }
        s_fill = 0;
    }
}

afe_event_t afe_poll_event(void)
{
    if (s_data == NULL) return AFE_EVENT_NONE;

    /* 边沿检测在**主循环侧**做 —— 只比对任务侧自增的计数器，
     * 不碰 AFE，所以既不会阻塞也不会刷屏。
     *
     * ⚠️ **先查 END**：判停是我们唯一关心的事件（session.c 只对 SPEECH_END 动作），
     *    优先交付它，避免被密集的 START 挤在后面。 */
    if (s_end_cnt != s_seen_end) {
        s_seen_end   = s_end_cnt;
        s_seen_start = s_start_cnt;   /* END 之前的 START 一并算已消费，
                                         免得下一轮冒出一个过期的 START */
        return AFE_EVENT_SPEECH_END;
    }
    if (s_start_cnt != s_seen_start) {
        s_seen_start = s_start_cnt;
        return AFE_EVENT_SPEECH_START;
    }
    return AFE_EVENT_NONE;
}

void afe_reset(void)
{
    s_fill = 0;

    /* 丢弃上一段录音遗留的事件，并让 fetch 任务重新建立基准 */
    s_seen_end   = s_end_cnt;
    s_seen_start = s_start_cnt;
    s_task_have  = false;

    if (s_data != NULL) {
        s_afe->reset_vad(s_data);
    }
}

const char *afe_vad_state_str(void)
{
    if (s_data == NULL)  return "not-wired";
    if (!s_task_have)    return "init";
    return s_speech ? "speech" : "silence";
}

#endif /* BOARD_HAS_ESPSR */
