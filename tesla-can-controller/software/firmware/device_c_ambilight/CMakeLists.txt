cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(device_c_ambilight)

# 设置目标芯片
set(COMPONENT_REQUIRES "")
set(COMPONENT_PRIV_REQUIRES "")

# 添加源文件
idf_component_register(SRCS "src/main.c"
                       "src/led_controller.c"
                       "src/ble_peripheral.c"
                       "src/light_effects.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver freertos nvs_flash esp_bt esp_ble_ack rmt led_strip)

# 设置分区表
set(PARTITION_TABLE_OFFSET 0x8000)

# 设置串口监控波特率
set(MONITOR_BAUD 115200)