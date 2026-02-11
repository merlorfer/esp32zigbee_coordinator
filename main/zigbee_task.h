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
 * @brief Request device to leave the Zigbee network
 *
 * Sends a ZDO leave request to the device, asking it to leave the network.
 * This should be called when removing a device from the device manager.
 *
 * @param ieee_addr Device IEEE address
 * @return ESP_OK on success
 */
esp_err_t zigbee_request_leave(uint64_t ieee_addr);

/**
 * @brief Reconfigure reporting for all sensors
 *
 * Sends configure_report commands to all sensors with retry mechanism.
 * This is useful to wake sleeping battery-powered sensors before entering BLE mode.
 *
 * @return ESP_OK on success
 */
esp_err_t zigbee_reconfigure_all_sensors(void);

/**
 * @brief Reconfigure reporting for a single sensor after config change
 *
 * @param ieee_addr Sensor IEEE address
 * @param device_type DEVICE_TYPE_TEMPERATURE_SENSOR or DEVICE_TYPE_HUMIDITY_SENSOR
 * @return ESP_OK on success
 */
esp_err_t zigbee_reconfigure_sensor(uint64_t ieee_addr, device_type_t device_type);

/**
 * @brief Read current data from all sensors
 *
 * Sends read_attribute commands to all sensors to get current values.
 * This is useful to refresh sensor data before entering BLE mode.
 *
 * @return ESP_OK on success
 */
esp_err_t zigbee_read_all_sensor_data(void);

#ifdef __cplusplus
}
#endif
