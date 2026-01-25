/**
 * @file ble_service.h
 * @brief BLE GATT service definitions for ESP32-C6 Zigbee Gateway
 */

#pragma once

#include "esp_err.h"
#include "host/ble_uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Service and Characteristic UUIDs
 * ============================================================================ */

// Main service UUID: 0000fff0-0000-1000-8000-00805f9b34fb
#define BLE_SVC_UUID16  0xFFF0

// Characteristics UUIDs
#define BLE_CHR_UUID_CMD_REQ    0xFFF1  // Command Request (Write)
#define BLE_CHR_UUID_CMD_RES    0xFFF2  // Command Response (Read + Notify)
#define BLE_CHR_UUID_DEV_LIST   0xFFF3  // Device List (Read + Notify)
#define BLE_CHR_UUID_SYS_STATUS 0xFFF4  // System Status (Read + Notify)
#define BLE_CHR_UUID_LARGE_XFER 0xFFF5  // Large Transfer (Write)

/* ============================================================================
 * BLE Service Functions
 * ============================================================================ */

/**
 * @brief Initialize BLE GATT services
 *
 * Registers GATT service and characteristics with NimBLE stack.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_service_init(void);

/**
 * @brief Send notification on Command Response characteristic
 *
 * @param data JSON response string
 * @param len Length of data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_service_notify_response(const char *data, size_t len);

/**
 * @brief Send notification on Device List characteristic
 *
 * @param data JSON device list string
 * @param len Length of data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_service_notify_devices(const char *data, size_t len);

/**
 * @brief Send notification on System Status characteristic
 *
 * @param data JSON status string
 * @param len Length of data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_service_notify_status(const char *data, size_t len);

/**
 * @brief Get current connection handle
 *
 * @return Connection handle, or BLE_HS_CONN_HANDLE_NONE if not connected
 */
uint16_t ble_service_get_conn_handle(void);

/**
 * @brief Set current connection handle
 *
 * @param conn_handle Connection handle
 */
void ble_service_set_conn_handle(uint16_t conn_handle);

/**
 * @brief Handle a subscription event.
 *
 * Called when a client subscribes to a characteristic. Sends initial data if applicable.
 *
 * @param conn_handle Connection handle of the subscribing client.
 * @param attr_handle Attribute handle of the characteristic value.
 */
void ble_service_on_subscribe(uint16_t conn_handle, uint16_t attr_handle);

#ifdef __cplusplus
}
#endif
