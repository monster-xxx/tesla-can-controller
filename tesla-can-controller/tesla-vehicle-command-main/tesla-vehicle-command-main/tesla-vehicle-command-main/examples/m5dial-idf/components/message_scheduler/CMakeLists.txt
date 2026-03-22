# Component CMakeLists.txt for Message Scheduler

idf_component_register(SRCS "message_scheduler.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos esp_timer
                       PRIV_REQUIRES )