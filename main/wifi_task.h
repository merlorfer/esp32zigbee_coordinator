/**
 * @file wifi_task.h
 * @brief Wi-Fi SoftAP and HTTP server task
 */

#pragma once

#include "common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Wi-Fi task
 * @return ESP_OK on success
 */
esp_err_t wifi_task_init(void);

/**
 * @brief Start Wi-Fi AP and HTTP server
 * @return ESP_OK on success
 */
esp_err_t wifi_task_start(void);

/**
 * @brief Stop Wi-Fi AP and HTTP server
 * @return ESP_OK on success
 */
esp_err_t wifi_task_stop(void);

/**
 * @brief Check if Wi-Fi AP is active
 * @return true if active
 */
bool wifi_task_is_active(void);

/**
 * @brief Set RTC time
 * @param year Year (e.g., 2025)
 * @param month Month (1-12)
 * @param day Day (1-31)
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 * @param second Second (0-59)
 * @return ESP_OK on success
 */
esp_err_t wifi_task_set_rtc(int year, int month, int day, int hour, int minute, int second);

/**
 * @brief Get current RTC time as string
 * @param buffer Buffer to store time string
 * @param buffer_size Size of buffer
 * @return ESP_OK on success
 */
esp_err_t wifi_task_get_rtc_string(char *buffer, size_t buffer_size);

/**
 * @brief Check if RTC is initialized
 * @return true if initialized
 */
bool wifi_task_is_rtc_initialized(void);

#ifdef __cplusplus
}
#endif
