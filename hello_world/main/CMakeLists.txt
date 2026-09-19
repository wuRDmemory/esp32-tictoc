idf_component_register(SRCS "hello_world_main.c"
                       # driver        -> uart_driver_install / uart_vfs_dev_*
                       # spi_flash     -> esp_flash_get_size (读真实 Flash 容量)
                       # esp_hw_support-> esp_read_mac
                       # esp_psram     -> esp_psram_is_initialized / esp_psram_get_size
                       PRIV_REQUIRES driver spi_flash esp_hw_support esp_psram
                       INCLUDE_DIRS "")
