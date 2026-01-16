/**
 * @file device_manager.h
 * @brief Device list management
 */

#pragma once

#include "common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize device manager
 * @return ESP_OK on success
 */
esp_err_t device_manager_init(void);

/**
 * @brief Add a new device
 * @param ieee_addr 64-bit IEEE address
 * @param endpoint Endpoint ID
 * @param manufacturer Manufacturer name (can be NULL)
 * @param model Model identifier (can be NULL)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if device limit reached
 */
esp_err_t device_manager_add(uint64_t ieee_addr, uint8_t endpoint,
                             const char *manufacturer, const char *model);

/**
 * @brief Remove a device by IEEE address
 * @param ieee_addr 64-bit IEEE address
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t device_manager_remove(uint64_t ieee_addr);

/**
 * @brief Get device by IEEE address
 * @param ieee_addr 64-bit IEEE address
 * @param device Pointer to store device configuration
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t device_manager_get(uint64_t ieee_addr, device_config_t *device);

/**
 * @brief Get device by index
 * @param index Device index
 * @param device Pointer to store device configuration
 * @return ESP_OK on success
 */
esp_err_t device_manager_get_by_index(uint8_t index, device_config_t *device);

/**
 * @brief Update device configuration
 * @param ieee_addr 64-bit IEEE address
 * @param device Pointer to new device configuration
 * @return ESP_OK on success
 */
esp_err_t device_manager_update(uint64_t ieee_addr, const device_config_t *device);

/**
 * @brief Update device state
 * @param ieee_addr 64-bit IEEE address
 * @param state New state (true=ON, false=OFF)
 * @return ESP_OK on success
 */
esp_err_t device_manager_set_state(uint64_t ieee_addr, bool state);

/**
 * @brief Get device count
 * @return Number of devices
 */
uint8_t device_manager_get_count(void);

/**
 * @brief Find device index by IEEE address
 * @param ieee_addr 64-bit IEEE address
 * @return Device index or -1 if not found
 */
int device_manager_find_index(uint64_t ieee_addr);

/**
 * @brief Check if device exists
 * @param ieee_addr 64-bit IEEE address
 * @return true if device exists
 */
bool device_manager_exists(uint64_t ieee_addr);

/**
 * @brief Set error for a device
 * @param ieee_addr 64-bit IEEE address
 * @param error_message Error message
 * @return ESP_OK on success
 */
esp_err_t device_manager_set_error(uint64_t ieee_addr, const char *error_message);

/**
 * @brief Clear error for a device
 * @param ieee_addr 64-bit IEEE address
 * @return ESP_OK on success
 */
esp_err_t device_manager_clear_error(uint64_t ieee_addr);

/**
 * @brief Get error for a device
 * @param ieee_addr 64-bit IEEE address
 * @param error Pointer to store error
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no error
 */
esp_err_t device_manager_get_error(uint64_t ieee_addr, device_error_t *error);

/**
 * @brief Clear all device errors
 * @return ESP_OK on success
 */
esp_err_t device_manager_clear_all_errors(void);

/**
 * @brief Save all devices to NVS
 * @return ESP_OK on success
 */
esp_err_t device_manager_save_all(void);

/**
 * @brief Power off all devices (send OFF command)
 */
void device_manager_power_off_all(void);

/**
 * @brief Get global configuration
 * @param config Pointer to store configuration
 * @return ESP_OK on success
 */
esp_err_t device_manager_get_global_config(global_config_t *config);

/**
 * @brief Set global configuration
 * @param config Pointer to configuration
 * @return ESP_OK on success
 */
esp_err_t device_manager_set_global_config(const global_config_t *config);

#ifdef __cplusplus
}
#endif
