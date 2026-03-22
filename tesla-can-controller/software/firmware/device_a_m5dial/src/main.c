/**
 * Tesla CAN Controller - Device A (M5Dial Main Controller)
 * 
 * Main firmware for M5Dial device serving as the primary user interface
 * and system controller for Tesla Model Y CAN control system.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// LVGL includes
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

// Project includes
#include "ui_manager.h"
#include "ble_central.h"
#include "vehicle_state.h"

static const char *TAG = "MAIN";

/**
 * @brief Initialize non-volatile storage
 */
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
}

/**
 * @brief Initialize LVGL graphics library
 */
static void init_lvgl(void)
{
    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();
    ESP_LOGI(TAG, "LVGL initialized");
}

/**
 * @brief Main UI task
 */
static void ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting UI task");
    
    // Initialize UI manager
    ui_manager_init();
    
    // Create main screen
    ui_manager_create_main_screen();
    
    // Main UI loop
    while (1) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief BLE communication task
 */
static void ble_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting BLE task");
    
    // Initialize BLE central
    ble_central_init();
    
    // Scan for CAN reader and ambient light controller
    ble_central_scan_devices();
    
    // Main BLE loop
    while (1) {
        ble_central_process();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Vehicle state monitoring task
 */
static void vehicle_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting vehicle state task");
    
    // Initialize vehicle state
    vehicle_state_init();
    
    // Main vehicle state loop
    while (1) {
        vehicle_state_update();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/**
 * @brief Application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Tesla CAN Controller - Device A (M5Dial) starting...");
    
    // Initialize system components
    init_nvs();
    init_lvgl();
    
    // Create tasks
    xTaskCreate(ui_task, "ui_task", 8192, NULL, 5, NULL);
    xTaskCreate(ble_task, "ble_task", 4096, NULL, 4, NULL);
    xTaskCreate(vehicle_task, "vehicle_task", 4096, NULL, 3, NULL);
    
    ESP_LOGI(TAG, "System initialized, tasks running");
    
    // Keep main task alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}