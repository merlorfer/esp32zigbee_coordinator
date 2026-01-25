/**
 * @file ble_service.c
 * @brief BLE GATT service implementation
 */

#include "ble_service.h"
#include "ble_handlers.h"
#include "common.h"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "BLE_SERVICE";

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_chr_val_handle_cmd_res;
static uint16_t s_chr_val_handle_dev_list;
static uint16_t s_chr_val_handle_sys_status;

// Chunked transfer state
#define MAX_CHUNK_SIZE 512
static char s_chunk_buffer[MAX_CHUNK_SIZE];
static size_t s_chunk_offset = 0;

/* ============================================================================
 * Characteristic Access Callbacks
 * ============================================================================ */

static int chr_access_cmd_req(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        ESP_LOGI(TAG, "Command request write; len=%d", OS_MBUF_PKTLEN(ctxt->om));

        // Extract command data
        char cmd_buf[512];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= sizeof(cmd_buf)) {
            len = sizeof(cmd_buf) - 1;
        }

        rc = ble_hs_mbuf_to_flat(ctxt->om, cmd_buf, len, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to copy command data; rc=%d", rc);
            return BLE_ATT_ERR_UNLIKELY;
        }
        cmd_buf[len] = '\0';

        ESP_LOGI(TAG, "Received command: %s", cmd_buf);

        // Process command
        char *response = ble_handlers_process_command(cmd_buf, len);
        if (response != NULL) {
            // Send response via notification
            ble_service_notify_response(response, strlen(response));
            free(response);
        } else {
            // Send error response
            const char *error_resp = "{\"status\":\"error\",\"message\":\"Command processing failed\"}";
            ble_service_notify_response(error_resp, strlen(error_resp));
        }

        return 0;

    default:
        ESP_LOGW(TAG, "Unexpected operation on cmd_req: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int chr_access_cmd_res(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        ESP_LOGI(TAG, "Command response read");
        // Response is sent via notifications, not direct reads
        return 0;

    default:
        ESP_LOGW(TAG, "Unexpected operation on cmd_res: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int chr_access_dev_list(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        ESP_LOGI(TAG, "Device list read");
        // Device list is sent via notifications
        return 0;

    default:
        ESP_LOGW(TAG, "Unexpected operation on dev_list: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int chr_access_sys_status(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        ESP_LOGI(TAG, "System status read");
        // Status is sent via notifications
        return 0;

    default:
        ESP_LOGW(TAG, "Unexpected operation on sys_status: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int chr_access_large_xfer(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        ESP_LOGI(TAG, "Large transfer write; len=%d, offset=%zu",
                 OS_MBUF_PKTLEN(ctxt->om), s_chunk_offset);

        // Extract chunk data
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (s_chunk_offset + len > MAX_CHUNK_SIZE) {
            ESP_LOGW(TAG, "Chunk buffer overflow");
            s_chunk_offset = 0;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        rc = ble_hs_mbuf_to_flat(ctxt->om, s_chunk_buffer + s_chunk_offset, len, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to copy chunk data; rc=%d", rc);
            s_chunk_offset = 0;
            return BLE_ATT_ERR_UNLIKELY;
        }

        s_chunk_offset += len;

        // Check if this is the end of transfer (determined by client sending smaller chunk or special marker)
        // For now, we process immediately if we have valid JSON
        s_chunk_buffer[s_chunk_offset] = '\0';

        // Try to parse as complete JSON
        if (s_chunk_buffer[s_chunk_offset - 1] == '}' || s_chunk_buffer[s_chunk_offset - 1] == ']') {
            ESP_LOGI(TAG, "Processing chunked command: %s", s_chunk_buffer);

            char *response = ble_handlers_process_command(s_chunk_buffer, s_chunk_offset);
            if (response != NULL) {
                ble_service_notify_response(response, strlen(response));
                free(response);
            } else {
                const char *error_resp = "{\"status\":\"error\",\"message\":\"Chunked command processing failed\"}";
                ble_service_notify_response(error_resp, strlen(error_resp));
            }

            s_chunk_offset = 0;
        }

        return 0;

    default:
        ESP_LOGW(TAG, "Unexpected operation on large_xfer: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ============================================================================
 * GATT Service Definition
 * ============================================================================ */

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        // Service: ESP32C6 Gateway
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Characteristic: Command Request
                .uuid = BLE_UUID16_DECLARE(BLE_CHR_UUID_CMD_REQ),
                .access_cb = chr_access_cmd_req,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // Characteristic: Command Response
                .uuid = BLE_UUID16_DECLARE(BLE_CHR_UUID_CMD_RES),
                .access_cb = chr_access_cmd_res,
                .val_handle = &s_chr_val_handle_cmd_res,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // Characteristic: Device List
                .uuid = BLE_UUID16_DECLARE(BLE_CHR_UUID_DEV_LIST),
                .access_cb = chr_access_dev_list,
                .val_handle = &s_chr_val_handle_dev_list,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // Characteristic: System Status
                .uuid = BLE_UUID16_DECLARE(BLE_CHR_UUID_SYS_STATUS),
                .access_cb = chr_access_sys_status,
                .val_handle = &s_chr_val_handle_sys_status,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // Characteristic: Large Transfer
                .uuid = BLE_UUID16_DECLARE(BLE_CHR_UUID_LARGE_XFER),
                .access_cb = chr_access_large_xfer,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                0, // No more characteristics
            }
        },
    },
    {
        0, // No more services
    },
};

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t ble_service_init(void)
{
    int rc;

    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count GATT config; rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add GATT services; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE GATT service initialized");
    return ESP_OK;
}

esp_err_t ble_service_notify_response(const char *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "No active connection for notification");
        return ESP_FAIL;
    }

    // Log response data for debugging
    ESP_LOGI(TAG, "Sending response: len=%d", (int)len);
    if (len < 200) {
        ESP_LOGI(TAG, "Response data: %.*s", (int)len, data);
    } else {
        ESP_LOGI(TAG, "Response data (first 200 bytes): %.*s...", 200, data);
    }

    struct os_mbuf *om;
    om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mbuf for notification");
        return ESP_FAIL;
    }

    int rc = ble_gattc_notify_custom(s_conn_handle, s_chr_val_handle_cmd_res, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to send notification; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Response notification sent successfully");
    return ESP_OK;
}

esp_err_t ble_service_notify_devices(const char *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "No active connection for notification");
        return ESP_FAIL;
    }

    struct os_mbuf *om;
    om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mbuf for notification");
        return ESP_FAIL;
    }

    int rc = ble_gattc_notify_custom(s_conn_handle, s_chr_val_handle_dev_list, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to send notification; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sent device list notification: len=%zu", len);
    return ESP_OK;
}

esp_err_t ble_service_notify_status(const char *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "No active connection for notification");
        return ESP_FAIL;
    }

    struct os_mbuf *om;
    om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mbuf for notification");
        return ESP_FAIL;
    }

    int rc = ble_gattc_notify_custom(s_conn_handle, s_chr_val_handle_sys_status, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to send notification; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sent status notification: len=%zu", len);
    return ESP_OK;
}

uint16_t ble_service_get_conn_handle(void)
{
    return s_conn_handle;
}

void ble_service_set_conn_handle(uint16_t conn_handle)
{
    s_conn_handle = conn_handle;
    ESP_LOGI(TAG, "Connection handle set to: 0x%04x", conn_handle);
}

void ble_service_on_subscribe(uint16_t conn_handle, uint16_t attr_handle)
{
    // Ensure we are handling the event for the currently active connection
    if (conn_handle != s_conn_handle) {
        return;
    }

    ESP_LOGI(TAG, "Handling subscribe event for attr_handle %d", attr_handle);

    // When a client subscribes, it's good practice to send the initial state.
    if (attr_handle == s_chr_val_handle_dev_list) {
        ESP_LOGI(TAG, "Client subscribed to Device List. Sending initial list.");
        const char *cmd = "{\"cmd\":\"get_devices\"}";
        char *response = ble_handlers_process_command(cmd, strlen(cmd));
        if (response) {
            ble_service_notify_devices(response, strlen(response));
            free(response);
        }
    } else if (attr_handle == s_chr_val_handle_sys_status) {
        ESP_LOGI(TAG, "Client subscribed to System Status. Sending initial status.");
        const char *cmd = "{\"cmd\":\"get_status\"}";
        char *response = ble_handlers_process_command(cmd, strlen(cmd));
        if (response) {
            ble_service_notify_status(response, strlen(response));
            free(response);
        }
    } else if (attr_handle == s_chr_val_handle_cmd_res) {
        ESP_LOGI(TAG, "Client subscribed to Command Response. No initial value to send.");
        // No initial data for command responses, they are event-driven.
    }
}
