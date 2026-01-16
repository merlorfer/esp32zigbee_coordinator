/**
 * @file zigbee_task.h
 * @brief Zigbee Coordinator task
 */

#pragma once

#include "common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Zigbee task
 * @return ESP_OK on success
 */
esp_err_t zigbee_task_init(void);

/**
 * @brief Start Zigbee coordinator
 * @return ESP_OK on success
 */
esp_err_t zigbee_task_start(void);

/**
 * @brief Enable permit join
 * @param duration Duration in seconds
 * @return ESP_OK on success
 */
esp_err_t zigbee_permit_join(uint8_t duration);

/**
 * @brief Send ON command to device
 * @param ieee_addr Device IEEE address
 * @param endpoint Device endpoint
 * @return ESP_OK on success
 */
esp_err_t zigbee_send_on(uint64_t ieee_addr, uint8_t endpoint);

/**
 * @brief Send OFF command to device
 * @param ieee_addr Device IEEE address
 * @param endpoint Device endpoint
 * @return ESP_OK on success
 */
esp_err_t zigbee_send_off(uint64_t ieee_addr, uint8_t endpoint);

/**
 * @brief Send TOGGLE command to device
 * @param ieee_addr Device IEEE address
 * @param endpoint Device endpoint
 * @return ESP_OK on success
 */
esp_err_t zigbee_send_toggle(uint64_t ieee_addr, uint8_t endpoint);

/**
 * @brief Check if Zigbee stack is running
 * @return true if running
 */
bool zigbee_is_running(void);

#ifdef __cplusplus
}
#endif
