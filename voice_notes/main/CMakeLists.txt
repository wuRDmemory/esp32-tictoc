idf_component_register(SRCS "mic_test.c"
                       # driver -> 兼容层，它 REQUIRE 了 esp_driver_i2s / esp_driver_uart
                       #           (见 components/driver/CMakeLists.txt:109,111)
                       PRIV_REQUIRES driver
                       INCLUDE_DIRS "")
