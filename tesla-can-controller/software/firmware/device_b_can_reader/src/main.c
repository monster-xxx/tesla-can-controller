/**
 * Tesla CAN Controller - Device B (CAN Reader)
 * 
 * Main firmware for ESP32-C6 device serving as CAN bus reader and
 * communication bridge between vehicle CAN network and M5Dial controller.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/twai.h"

// Project includes
#include "can_handler.h"
#include "ble_peripheral.h"
#include "tesla_dbc_parser.h"

static const char *TAG = "CAN_READER";

// CAN configuration
#define CAN_TX_GPIO_NUM   21
#define CAN_RX_GPIO_NUM   22
#define CAN_SPEED         TWAI_TIMING_CONFIG_500KBITS

// Message queues
QueueHandle_t can_rx_queue;
QueueHandle_t ble_tx_queue;

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
 * @brief Initialize CAN controller
 */
static void init_can(void)
{
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO_NUM, CAN_RX_GPIO_NUM, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = CAN_SPEED;
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(TAG, "CAN controller initialized at 500kbps");
}

/**
 * @brief CAN reception task
 */
static void can_rx_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting CAN RX task");
    
    twai_message_t message;
    
    while (1) {
        // Receive CAN message
        if (twai_receive(&message, pdMS_TO_TICKS(100)) == ESP_OK) {
            // Process CAN message
            can_handler_process_message(&message);
            
            // Parse Tesla DBC if applicable
            tesla_dbc_parse_message(&message);
            
            // Queue for BLE transmission
            if (ble_tx_queue != NULL) {
                // Create BLE packet from CAN message
                ble_packet_t packet;
                if (can_handler_create_ble_packet(&message, &packet)) {
                    xQueueSend(ble_tx_queue, &packet, portMAX_DELAY);
                }
            }
        }
    }
}

/**
 * @brief CAN transmission task
 */
static void can_tx_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting CAN TX task");
    
    twai_message_t message;
    
    while (1) {
        // Check for messages to transmit (from BLE)
        // This would typically come from a queue
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief BLE communication task
 */
static void ble_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting BLE task");
    
    // Initialize BLE peripheral
    ble_peripheral_init();
    
    // Advertise and wait for connection
    ble_peripheral_start_advertising();
    
    // Main BLE loop
    while (1) {
        ble_peripheral_process();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief Application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Tesla CAN Controller - Device B (CAN Reader) starting...");
    
    // Initialize system components
    init_nvs();
    init_can();
    
    // Create message queues
    can_rx_queue = xQueueCreate(20, sizeof(twai_message_t));
    ble_tx_queue = xQueueCreate(20, sizeof(ble_packet_t));
    
    // Create tasks
    xTaskCreate(can_rx_task, "can_rx_task", 4096, NULL, 5, NULL);
    xTaskCreate(can_tx_task, "can_tx_task", 4096, NULL, 4, NULL);
    xTaskCreate(ble_task, "ble_task", 4096, NULL, 3, NULL);
    
    ESP_LOGI(TAG, "System initialized, tasks running");
    
    // Keep main task alive
    while (1) {
        // Monitor system health
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Log status periodically
        static int counter = 0;
        if (++counter % 10 == 0) {
            twai_status_info_t status;
            twai_get_status_info(&status);
            ESP_LOGI(TAG, "CAN status: msgs_rx=%lu, msgs_tx=%lu, state=%d", 
                    status.msgs_rx, status.msgs_tx, status.state);
        }
    }
}