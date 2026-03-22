/**
 * UI Manager for Tesla CAN Controller - Device A (M5Dial)
 * 
 * Handles LVGL-based user interface including screens, menus, and interactions.
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Screen IDs
typedef enum {
    SCREEN_BOOT = 0,
    SCREEN_MAIN,
    SCREEN_VEHICLE_INFO,
    SCREEN_SETTINGS,
    SCREEN_PERFORMANCE,
    SCREEN_LIGHTS,
    SCREEN_MAX
} screen_id_t;

// Menu items
typedef enum {
    MENU_VEHICLE_INFO = 0,
    MENU_ESP_OFF,
    MENU_ACCELERATION_TEST,
    MENU_NAVIGATION_LIGHTS,
    MENU_PULSE_HIGH_BEAM,
    MENU_AMBIENT_LIGHTS,
    MENU_SETTINGS,
    MENU_MAX
} menu_item_t;

/**
 * @brief Initialize UI manager
 */
void ui_manager_init(void);

/**
 * @brief Create main screen
 */
void ui_manager_create_main_screen(void);

/**
 * @brief Switch to specified screen
 * @param screen Screen ID to switch to
 */
void ui_manager_switch_screen(screen_id_t screen);

/**
 * @brief Update vehicle information on UI
 * @param speed Current vehicle speed (km/h)
 * @param soc State of charge (%)
 * @param gear Current gear position
 * @param temp Battery temperature (°C)
 */
void ui_manager_update_vehicle_info(float speed, uint8_t soc, uint8_t gear, float temp);

/**
 * @brief Update performance test results
 * @param time_0_100 0-100km/h time in seconds
 * @param distance Distance in meters
 * @param max_g Max G-force recorded
 */
void ui_manager_update_performance_results(float time_0_100, float distance, float max_g);

/**
 * @brief Show confirmation dialog for critical operations
 * @param title Dialog title
 * @param message Dialog message
 * @param confirm_callback Callback when confirmed
 * @param cancel_callback Callback when cancelled
 */
void ui_manager_show_confirmation(const char *title, const char *message,
                                  void (*confirm_callback)(void),
                                  void (*cancel_callback)(void));

/**
 * @brief Show notification message
 * @param message Notification text
 * @param duration_ms Duration in milliseconds (0 for manual dismiss)
 */
void ui_manager_show_notification(const char *message, uint32_t duration_ms);

/**
 * @brief Set menu item state
 * @param item Menu item ID
 * @param enabled Whether item is enabled
 * @param active Whether item is active/selected
 */
void ui_manager_set_menu_state(menu_item_t item, bool enabled, bool active);

/**
 * @brief Set brightness level
 * @param brightness Brightness level (0-100%)
 */
void ui_manager_set_brightness(uint8_t brightness);

/**
 * @brief Set theme color
 * @param primary Primary color (RGB888)
 * @param secondary Secondary color (RGB888)
 */
void ui_manager_set_theme(uint32_t primary, uint32_t secondary);

/**
 * @brief Handle rotary encoder input
 * @param delta Encoder delta (positive for clockwise, negative for counter-clockwise)
 * @param pressed Whether button is pressed
 */
void ui_manager_handle_encoder(int32_t delta, bool pressed);

/**
 * @brief Handle touch input
 * @param x Touch X coordinate
 * @param y Touch Y coordinate
 * @param pressed Whether touch is pressed
 */
void ui_manager_handle_touch(int16_t x, int16_t y, bool pressed);

#ifdef __cplusplus
}
#endif

#endif // UI_MANAGER_H