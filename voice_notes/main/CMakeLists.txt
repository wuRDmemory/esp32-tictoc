idf_component_register(SRCS "mic_test.c" "i2s_mic.c" "audio_frame.c" "transport.c"
                       # driver     -> 兼容层，REQUIRE 了 esp_driver_i2s / esp_driver_uart
                       #               (见 components/driver/CMakeLists.txt:109,111)
                       # esp_timer  -> esp_timer_get_time（统计 TX 阻塞时长）
                       PRIV_REQUIRES driver esp_timer
                       INCLUDE_DIRS "")
