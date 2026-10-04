idf_component_register(SRCS "app_main.c" "session.c" "transport.c"
                            "i2s_mic.c" "audio_frame.c" "diag.c"
                            # AFE 能力抽象：**两份实现都列出**，由文件内
                            # `#if BOARD_HAS_ESPSR` 决定谁生效（另一份编译成空）。
                            # 这样"选哪块板"只有 board.h 一个出处，CMake 不必重复判断。
                            "afe_passthrough.c" "afe_espsr.c"
                       # driver     -> 兼容层，REQUIRE 了 esp_driver_i2s / esp_driver_uart
                       #               (见 components/driver/CMakeLists.txt:109,111)
                       # esp_timer  -> esp_timer_get_time（TX 阻塞统计、音频时间戳）
                       PRIV_REQUIRES driver esp_timer
                       INCLUDE_DIRS "")
