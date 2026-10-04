# esp-sr 只给 S3，所以依赖列表要先算好再注册。
#
# ⚠️ 这里为什么用 Kconfig 而不是 board.h：**CMake 读不到 C 头文件**。
#    `CONFIG_BOARD_S3_CAM` 与 board.h 的 `BOARD_HAS_ESPSR` 是**同一个选择的
#    两种写法**（都由 Kconfig.projbuild 的 BOARD_SELECT 派生），不是两套判断。
#
# ⚠️ 不加这个条件的话：给 PICO 构建时会因为找不到 esp-sr 组件直接报错，
#    而 PICO 上 afe_espsr.c 本来就会 `#if BOARD_HAS_ESPSR` 编译成空文件。
set(priv_reqs
    driver      # 兼容层，REQUIRE 了 esp_driver_i2s / esp_driver_uart
                # (见 components/driver/CMakeLists.txt:109,111)
    esp_timer   # esp_timer_get_time（TX 阻塞统计、音频时间戳）
)
if(CONFIG_BOARD_S3_CAM)
    list(APPEND priv_reqs esp-sr)   # ESP-SR 的 AFE（板端 VAD 判停，D13）
endif()

idf_component_register(SRCS "app_main.c" "session.c" "transport.c"
                            "i2s_mic.c" "audio_frame.c" "diag.c"
                            # AFE 能力抽象：**两份实现都列出**，由文件内
                            # `#if BOARD_HAS_ESPSR` 决定谁生效（另一份编译成空）。
                            # 这样"选哪块板"只有 board.h 一个出处，CMake 不必重复判断。
                            "afe_passthrough.c" "afe_espsr.c"
                       PRIV_REQUIRES ${priv_reqs}
                       INCLUDE_DIRS "")
