cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(device_a_m5dial)

# 设置目标芯片
set(COMPONENT_REQUIRES m5dial)
set(COMPONENT_PRIV_REQUIRES "")

# 添加源文件
idf_component_register(SRCS "src/main.c"
                       "src/ui_manager.c"
                       "src/ble_central.c"
                       "src/vehicle_state.c"
                       INCLUDE_DIRS "include"
                       REQUIRES m5dial lvgl freertos nvs_flash esp_bt esp_ble_ack)

# 设置分区表
set(PARTITION_TABLE_OFFSET 0x8000)

# 设置串口监控波特率
set(MONITOR_BAUD 115200)