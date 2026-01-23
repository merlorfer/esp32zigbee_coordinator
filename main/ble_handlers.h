/**
 * @file ble_handlers.h
 * @brief BLE command handlers for ESP32-C6 Zigbee Gateway
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Process incoming BLE command
 *
 * Parses JSON command and routes to appropriate handler.
 * Supported commands:
 * - get_status: System status
 * - get_devices: Device list
 * - set_rtc: Time synchronization
 * - set_device_config: Device automation configuration
 * - delete_device: Remove device (with Zigbee leave request)
 * - control_device: Send ON/OFF/TOGGLE
 * - permit_join: Enable pairing
 * - set_global_settings: Global automation settings
 *
 * @param json_str JSON command string
 * @param len Length of command string
 * @return JSON response string (must be freed by caller), or NULL on error
 */
char *ble_handlers_process_command(const char *json_str, size_t len);

#ifdef __cplusplus
}
#endif
