idf_component_register(SRCS "app_main.c" "session.c" "transport.c"
                            "i2s_mic.c" "audio_frame.c" "diag.c"
                       # driver     -> 兼容层，REQUIRE 了 esp_driver_i2s / esp_driver_uart
                       #               (见 components/driver/CMakeLists.txt:109,111)
                       # esp_timer  -> esp_timer_get_time（TX 阻塞统计、音频时间戳）
                       PRIV_REQUIRES driver esp_timer
                       INCLUDE_DIRS "")
