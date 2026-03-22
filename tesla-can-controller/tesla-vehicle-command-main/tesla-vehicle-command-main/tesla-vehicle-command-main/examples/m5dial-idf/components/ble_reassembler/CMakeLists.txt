# Component CMakeLists.txt for BLE Reassembler

idf_component_register(SRCS "ble_reassembler.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer freertos
                       PRIV_REQUIRES )