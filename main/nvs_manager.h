/**
 * @file nvs_manager.h
 * @brief NVS operations for ESP32-C6 Zigbee Gateway
 */

#pragma once

#include "common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * NVS Namespace Definitions
 * ============================================================================ */

#define NVS_NAMESPACE_CONFIG    "config"
#define NVS_NAMESPACE_DEVICES   "devices"
#define NVS_NAMESPACE_ERRORS    "errors"

/* ============================================================================
 * NVS Key Definitions
 * ============================================================================ */

#define NVS_KEY_GLOBAL          "global"
#define NVS_KEY_WIFI_BEHAVIOR   "wifi_on_beh"
#define NVS_KEY_DEVICE_COUNT    "count"
#define NVS_KEY_DEVICE_PREFIX   "dev_"
#define NVS_KEY_ERROR_PREFIX    "err_"

/* ============================================================================
 * Function Declarations
 * ============================================================================ */

/**
 * @brief Initialize NVS subsystem
 * @return ESP_OK on success
 */
esp_err_t nvs_manager_init(void);

/**
 * @brief Save global configuration to NVS
 * @param config Pointer to global configuration
 * @return ESP_OK on success
 */
esp_err_t nvs_save_global_config(const global_config_t *config);

/**
 * @brief Load global configuration from NVS
 * @param config Pointer to store loaded configuration
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not saved yet
 */
esp_err_t nvs_load_global_config(global_config_t *config);

/**
 * @brief Save device configuration to NVS
 * @param index Device index (0 to MAX_DEVICES-1)
 * @param device Pointer to device configuration
 * @return ESP_OK on success
 */
esp_err_t nvs_save_device(uint8_t index, const device_config_t *device);

/**
 * @brief Load device configuration from NVS
 * @param index Device index (0 to MAX_DEVICES-1)
 * @param device Pointer to store loaded configuration
 * @return ESP_OK on success
 */
esp_err_t nvs_load_device(uint8_t index, device_config_t *device);

/**
 * @brief Delete device from NVS
 * @param index Device index to delete
 * @return ESP_OK on success
 */
esp_err_t nvs_delete_device(uint8_t index);

/**
 * @brief Save device error to NVS
 * @param index Device index
 * @param error Pointer to error structure
 * @return ESP_OK on success
 */
esp_err_t nvs_save_error(uint8_t index, const device_error_t *error);

/**
 * @brief Load device error from NVS
 * @param index Device index
 * @param error Pointer to store loaded error
 * @return ESP_OK on success
 */
esp_err_t nvs_load_error(uint8_t index, device_error_t *error);

/**
 * @brief Clear all errors from NVS
 * @return ESP_OK on success
 */
esp_err_t nvs_clear_all_errors(void);

/**
 * @brief Save device count to NVS
 * @param count Number of devices
 * @return ESP_OK on success
 */
esp_err_t nvs_save_device_count(uint8_t count);

/**
 * @brief Load device count from NVS
 * @param count Pointer to store device count
 * @return ESP_OK on success
 */
esp_err_t nvs_load_device_count(uint8_t *count);

/* ============================================================================
 * Rules Engine NVS Functions
 * ============================================================================ */

/**
 * @brief Save rules text to NVS
 * @param text Rules text string
 * @param len Length of text (excluding null terminator)
 * @return ESP_OK on success
 */
esp_err_t nvs_save_rules_text(const char *text, size_t len);

/**
 * @brief Load rules text from NVS
 * @param text Buffer to store loaded text
 * @param len Pointer to buffer size (in), actual length loaded (out)
 * @return ESP_OK on success
 */
esp_err_t nvs_load_rules_text(char *text, size_t *len);

/**
 * @brief Save rules variables to NVS
 * @param vars Array of float variables
 * @param count Number of variables
 * @return ESP_OK on success
 */
esp_err_t nvs_save_rules_vars(const float *vars, uint8_t count);

/**
 * @brief Load rules variables from NVS
 * @param vars Array to store variables
 * @param count Number of variables to load
 * @return ESP_OK on success
 */
esp_err_t nvs_load_rules_vars(float *vars, uint8_t count);

/**
 * @brief Save rules variable config (persist mask + defaults) to NVS
 * @param persist_mask Bitmask: bit i = 1 means var[i] is persistent
 * @param defaults Array of default values for non-persistent variables
 * @param count Number of variables
 * @return ESP_OK on success
 */
esp_err_t nvs_save_rules_var_config(uint8_t persist_mask, const float *defaults, uint8_t count);

/**
 * @brief Load rules variable config from NVS
 * @param persist_mask Output bitmask
 * @param defaults Output array of default values
 * @param count Number of variables
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not stored yet
 */
esp_err_t nvs_load_rules_var_config(uint8_t *persist_mask, float *defaults, uint8_t count);

/**
 * @brief Erase all rules data from NVS (factory reset)
 * @return ESP_OK on success
 */
esp_err_t nvs_erase_rules(void);

#ifdef __cplusplus
}
#endif
