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

/**
 * @brief Stop Zigbee radio operation
 *
 * Stops Zigbee operations to allow Wi-Fi to use the shared radio.
 * Note: On ESP32-C6, Wi-Fi and Zigbee share the same 2.4GHz radio.
 *
 * @return ESP_OK on success
 */
esp_err_t zigbee_task_stop(void);

/**
 * @brief Resume Zigbee radio operation
 *
 * Resumes Zigbee operations after Wi-Fi mode.
 * Re-enables the IEEE 802.15.4 radio.
 *
 * @return ESP_OK on success
 */
esp_err_t zigbee_task_resume(void);

/**
 * @brief Request device to leave the Zigbee network
 *
 * Sends a ZDO leave request to the device, asking it to leave the network.
 * This should be called when removing a device from the device manager.
 *
 * @param ieee_addr Device IEEE address
 * @return ESP_OK on success
 */
esp_err_t zigbee_request_leave(uint64_t ieee_addr);

#ifdef __cplusplus
}
#endif
