/**
 * @file ble_task.h
 * @brief BLE lifecycle management for ESP32-C6 Zigbee Gateway
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize BLE task resources
 *
 * Initializes NimBLE stack and prepares BLE service.
 * Must be called before ble_task_start().
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_task_init(void);

/**
 * @brief Start BLE advertising and GATT server
 *
 * Starts BLE advertising with device name "ESP32C6_Gateway".
 * Makes device discoverable for Web Bluetooth connections.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_task_start(void);

/**
 * @brief Stop BLE advertising and GATT server
 *
 * Stops advertising and disconnects all BLE clients.
 * Can be called multiple times safely.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_task_stop(void);

/**
 * @brief Check if BLE is currently active
 *
 * @return true if BLE is advertising, false otherwise
 */
bool ble_task_is_active(void);

/**
 * @brief Check if RTC has been initialized
 *
 * @return true if RTC time is set, false otherwise
 */
bool ble_task_is_rtc_initialized(void);

/**
 * @brief Set RTC time
 *
 * @param year Full year (e.g., 2025)
 * @param month Month (1-12)
 * @param day Day of month (1-31)
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 * @param second Second (0-59)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_task_set_rtc(int year, int month, int day, int hour, int minute, int second);

/**
 * @brief Get RTC time as formatted string
 *
 * @param buffer Output buffer for time string
 * @param buffer_size Size of output buffer (min 20 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_task_get_rtc_string(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
