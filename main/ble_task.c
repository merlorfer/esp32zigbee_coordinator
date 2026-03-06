/**
 * @file ble_task.c
 * @brief BLE lifecycle management implementation
 */

#include "ble_task.h"
#include "ble_service.h"
#include "common.h"
#include "device_manager.h"
#include "nvs_manager.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "BLE_TASK";

static bool s_ble_active = false;
static bool s_rtc_initialized = false;
static uint8_t s_own_addr_type;

// External declarations
extern EventGroupHandle_t g_event_group;

// Forward declarations
static void ble_advertise(void);

/* ============================================================================
 * NimBLE Event Handlers
 * ============================================================================ */

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    if (event->type != 13) { // Skip logging for subscribe events to reduce log spam
        ESP_LOGI(TAG, "GAP event: %d", event->type);
    }
    
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);

        if (event->connect.status == 0) {
            // Connection established
            ble_service_set_conn_handle(event->connect.conn_handle);
            /* STABILITY FIX for Windows: Do not aggressively update connection parameters.
             * Let the Central (Windows/Phone) dictate the parameters to avoid conflicts.
             */
            /*
            // Update connection parameters for stability
            struct ble_gap_upd_params params = {
                .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,    // 30ms min interval
                .itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX,    // 50ms max interval
                .latency = 0,                                  // No latency
                .supervision_timeout = 500,                    // 5000ms (5 sec) timeout
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            int rc = ble_gap_update_params(event->connect.conn_handle, &params);
            if (rc != 0) {
                ESP_LOGW(TAG, "Failed to update connection params; rc=%d", rc);
            }
            */
        } else {
            // Connection failed; resume advertising
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnect; reason=%d (0x%02x)",
                 event->disconnect.reason, event->disconnect.reason);

        // Clear connection handle
        ble_service_set_conn_handle(BLE_HS_CONN_HANDLE_NONE);

        // Resume advertising if still active
        if (s_ble_active) {
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "BLE connection updated; status=%d", event->conn_update.status);
        break;
    
    case BLE_GAP_EVENT_SUBSCRIBE:
        // Log the subscription event with more readable output
        ESP_LOGI(TAG, "BLE subscribe event: conn_handle=%d, attr_handle=%d, notify=%s, indicate=%s",
                    event->subscribe.conn_handle,
                    event->subscribe.attr_handle,
                    event->subscribe.cur_notify ? "ENABLED" : "DISABLED",
                    event->subscribe.cur_indicate ? "ENABLED" : "DISABLED");

        // If a client is enabling notifications, handle it by sending the initial state.
        if (event->subscribe.cur_notify) {
            ble_service_on_subscribe(event->subscribe.conn_handle, event->subscribe.attr_handle);
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE advertise complete; reason=%d", event->adv_complete.reason);

        // Resume advertising if still active
        if (s_ble_active) {
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU update; conn_handle=%d, mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        ESP_LOGI(TAG, "BLE connection update request");
        // Just return 0 to accept the parameters requested by the peer
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        // Notification transmission complete
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption change; status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Allow re-pairing if the client lost its keys
        ESP_LOGI(TAG, "BLE repeat pairing request - deleting old bond");
        return 0; // 0 = allow

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        // Safe to ignore these optimization events
        return 0;

    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        ESP_LOGI(TAG, "BLE identity resolved - Device successfully paired");
        return 0;

    default:
        ESP_LOGW(TAG, "Unhandled GAP event: %d", event->type);
        break;
    }

    return 0;
}

/* ============================================================================
 * BLE Advertising
 * ============================================================================ */

static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *device_name = "ESP32C6_Gateway";
    int rc;

    // Set advertisement data
    memset(&fields, 0, sizeof(fields));

    // Advertise device name
    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;

    // General discoverable flag
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Include service UUID in advertisement
    ble_uuid16_t service_uuid = BLE_UUID16_INIT(BLE_SVC_UUID16);
    fields.uuids16 = &service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertisement data; rc=%d", rc);
        return;
    }

    // Start advertising
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising; rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started: %s", device_name);
}

/* ============================================================================
 * NimBLE Host Task
 * ============================================================================ */

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset; reason=%d", reason);
}

static void ble_on_sync(void)
{
    int rc;

    ESP_LOGI(TAG, "BLE host synced");

    // Determine best address type
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address; rc=%d", rc);
        return;
    }

    // Figure out address to use
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type; rc=%d", rc);
        return;
    }

    // Set preferred MTU to support larger notifications
    rc = ble_att_set_preferred_mtu(512);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to set preferred MTU; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Preferred MTU set to 512 bytes");
    }

    // Print BLE address
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE address: %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0]);
    }

    // Start advertising if BLE is active
    if (s_ble_active) {
        ble_advertise();
    }
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t ble_task_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing NimBLE stack");

    // Initialize NVP (Non-Volatile Platform) - required by NimBLE
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize nimble port; ret=%d", ret);
        return ret;
    }

    // Suppress verbose NimBLE internal logs (GATT notify spam etc.)
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // Initialize BLE host configuration
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;

    // Security settings - Enable "Just Works" bonding to satisfy Windows requirements
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;  // Enable bonding
    ble_hs_cfg.sm_mitm = 0;     // No Man-In-The-Middle protection
    ble_hs_cfg.sm_sc = 1;       // Prefer Secure Connections

    // Set device name
    ret = ble_svc_gap_device_name_set("ESP32C6_Gateway");
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set device name; ret=%d", ret);
        return ESP_FAIL;
    }

    // Initialize GATT service
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Initialize custom GATT service
    ret = ble_service_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE service");
        return ret;
    }

    // Start NimBLE host task
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE task initialized");
    return ESP_OK;
}

esp_err_t ble_task_start(void)
{
    if (s_ble_active) {
        ESP_LOGW(TAG, "BLE already active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting BLE");

    s_ble_active = true;

    // If BLE host is already synced, start advertising now
    if (ble_hs_synced()) {
        ble_advertise();
    }
    // Otherwise, advertising will start automatically in ble_on_sync()

    xEventGroupSetBits(g_event_group, EVENT_BLE_MODE_BIT);

    ESP_LOGI(TAG, "BLE started");
    return ESP_OK;
}

esp_err_t ble_task_stop(void)
{
    if (!s_ble_active) {
        ESP_LOGW(TAG, "BLE already inactive");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping BLE");

    // Stop advertising
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Failed to stop advertising; rc=%d", rc);
    }

    // Disconnect all connections
    uint16_t conn_handle = ble_service_get_conn_handle();
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    s_ble_active = false;
    xEventGroupClearBits(g_event_group, EVENT_BLE_MODE_BIT);

    ESP_LOGI(TAG, "BLE stopped");
    return ESP_OK;
}

bool ble_task_is_active(void)
{
    return s_ble_active;
}

esp_err_t ble_task_set_rtc(int year, int month, int day, int hour, int minute, int second)
{
    struct tm tm_time = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second
    };

    time_t t = mktime(&tm_time);
    struct timeval tv = {
        .tv_sec = t,
        .tv_usec = 0
    };

    settimeofday(&tv, NULL);
    s_rtc_initialized = true;

    xEventGroupSetBits(g_event_group, EVENT_RTC_INIT_BIT);

    // Update global config
    global_config_t config;
    device_manager_get_global_config(&config);
    config.rtc_initialized = true;
    config.last_rtc_set = (uint32_t)t;
    device_manager_set_global_config(&config);

    ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);

    return ESP_OK;
}

esp_err_t ble_task_get_rtc_string(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size < 20) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);

    return ESP_OK;
}

bool ble_task_is_rtc_initialized(void)
{
    return s_rtc_initialized;
}
