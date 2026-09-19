# ESP32-PICO-KIT V4 hello_world
# 工程入口: 交给 ESP-IDF 的 project.cmake 处理，不要在这里写业务逻辑
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(hello_world)
