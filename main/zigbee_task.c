/**
 * @file zigbee_task.c
 * @brief Zigbee Coordinator task implementation
 */

#include "zigbee_task.h"
#include "device_manager.h"
#include "led_task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "esp_ieee802154.h"
#include <string.h>
#include <inttypes.h>
#include <time.h>

/* Helper function to format IEEE address as hex string */
static void format_ieee_addr(char *buf, size_t buf_size, const uint8_t *ieee_addr)
{
    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
             ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);
}

/* Helper function to convert IEEE address bytes to uint64 */
static uint64_t ieee_to_uint64(const uint8_t *ieee_addr)
{
    uint64_t result;
    // Use memcpy to avoid byte order issues
    memcpy(&result, ieee_addr, sizeof(uint64_t));
    return result;
}

/* Helper function to convert uint64 to IEEE address bytes */
static void uint64_to_ieee(uint64_t addr, uint8_t *ieee_addr)
{
    memcpy(ieee_addr, &addr, sizeof(uint64_t));
}

/* Helper function to format uint64 IEEE address as hex string */
static void format_ieee_addr_u64(char *buf, size_t buf_size, uint64_t ieee_addr)
{
    uint8_t ieee_bytes[8];
    uint64_to_ieee(ieee_addr, ieee_bytes);
    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             ieee_bytes[7], ieee_bytes[6], ieee_bytes[5], ieee_bytes[4],
             ieee_bytes[3], ieee_bytes[2], ieee_bytes[1], ieee_bytes[0]);
}

/* Zigbee configuration */
#define INSTALLCODE_POLICY_ENABLE       false      /* enable the install code policy for security */
#define HA_GATEWAY_ENDPOINT             1          /* esp gateway device endpoint */

/* Zigbee Coordinator configuration macro */
#define ESP_ZB_ZC_CONFIG()                                                              \
    {                                                                                   \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,                                  \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,                               \
        .nwk_cfg.zczr_cfg = {                                                           \
            .max_children = MAX_DEVICES,                                                \
        },                                                                              \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()                           \
    {                                                           \
        .radio_mode = ZB_RADIO_MODE_NATIVE,                     \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                            \
    {                                                           \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,   \
    }

static const char *TAG = "ZIGBEE_TASK";

// External declarations
extern EventGroupHandle_t g_event_group;
extern QueueHandle_t g_cmd_queue;

static bool s_zigbee_running = false;
static TaskHandle_t s_zigbee_task_handle = NULL;

// Pending device discovery tracking
typedef struct {
    uint16_t short_addr;
    uint64_t ieee_addr;
    bool pending;
    uint8_t expected_responses;  // Number of simple descriptor responses we're waiting for
    bool match_desc_attempted;   // True if match_desc fallback was already tried (prevents loop)
    bool find_light_attempted;   // True if esp_zb_zdo_find_on_off_light was already tried
} pending_discovery_t;

static pending_discovery_t s_pending_discovery = {0};

// Forward declarations for device discovery
static void find_on_off_device(esp_zb_zdo_match_desc_req_param_t *param, esp_zb_zdo_match_desc_callback_t user_cb, void *user_ctx);
static void user_find_on_off_cb(esp_zb_zdp_status_t zdo_status, uint16_t peer_addr, uint8_t peer_endpoint, void *user_ctx);
static void request_active_endpoints(uint16_t short_addr);
static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_list, void *user_ctx);
static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx);
static void configure_sensor_reporting(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id,
                                       uint16_t min_interval, uint16_t max_interval, int16_t report_change_val);

// IEEE address resolution for short addresses unknown after coordinator reboot
#define MAX_IEEE_PENDING 8
static uint16_t s_ieee_pending_shorts[MAX_IEEE_PENDING];
static uint8_t s_ieee_pending_count = 0;

static void ieee_addr_resolved_cb(esp_zb_zdp_status_t zdo_status, esp_zb_zdo_ieee_addr_rsp_t *resp, void *user_ctx)
{
    uint16_t short_addr = (uint16_t)(uintptr_t)user_ctx;

    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS && resp) {
        uint64_t ieee_addr = ieee_to_uint64(resp->ieee_addr);
        char ieee_str[24];
        format_ieee_addr_u64(ieee_str, sizeof(ieee_str), ieee_addr);
        ESP_LOGI(TAG, "IEEE address resolved: short 0x%04x -> %s", short_addr, ieee_str);
    } else {
        ESP_LOGW(TAG, "IEEE address request failed for short 0x%04x, status=%d", short_addr, zdo_status);
    }

    // Remove from pending list
    for (uint8_t i = 0; i < s_ieee_pending_count; i++) {
        if (s_ieee_pending_shorts[i] == short_addr) {
            s_ieee_pending_shorts[i] = s_ieee_pending_shorts[--s_ieee_pending_count];
            break;
        }
    }
}

static void request_ieee_for_short(uint16_t short_addr)
{
    // Check if already pending
    for (uint8_t i = 0; i < s_ieee_pending_count; i++) {
        if (s_ieee_pending_shorts[i] == short_addr) return;
    }

    // Add to pending list
    if (s_ieee_pending_count < MAX_IEEE_PENDING) {
        s_ieee_pending_shorts[s_ieee_pending_count++] = short_addr;
    } else {
        return; // List full, skip
    }

    esp_zb_zdo_ieee_addr_req_param_t req = {
        .dst_nwk_addr = short_addr,
        .addr_of_interest = short_addr,
        .request_type = 0,
        .start_index = 0
    };

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zdo_ieee_addr_req(&req, ieee_addr_resolved_cb, (void *)(uintptr_t)short_addr);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "Requesting IEEE address for short 0x%04x", short_addr);
}

// Callback to clear pending discovery after timeout
// param=0: simple_desc phase timeout (or stale alarm after match_desc already started)
// param=1: match_desc phase timeout — final give-up
static void discovery_timeout_cb(uint8_t param)
{
    if (!s_pending_discovery.pending) {
        return;
    }

    char ieee_str[24];
    format_ieee_addr_u64(ieee_str, sizeof(ieee_str), s_pending_discovery.ieee_addr);

    if (param == 2) {
        // find_on_off_light phase timed out — final give up
        ESP_LOGW(TAG, "find_on_off_light timed out for %s - device not supported", ieee_str);
        s_pending_discovery.pending = false;
        s_pending_discovery.expected_responses = 0;
        return;
    }

    if (param == 1) {
        if (s_pending_discovery.find_light_attempted) {
            // find_on_off_light is in progress — this is the stale match_desc alarm,
            // param=2 alarm will handle the final timeout
            return;
        }
        // match_desc phase timed out — give up
        ESP_LOGW(TAG, "Match descriptor timed out for %s - device not supported", ieee_str);
        s_pending_discovery.pending = false;
        s_pending_discovery.expected_responses = 0;
        return;
    }

    // param == 0
    if (s_pending_discovery.match_desc_attempted) {
        // match_desc was already started (by simple_desc_cb error path).
        // This is the stale param=0 alarm from active_ep_cb — set a fresh match_desc timeout.
        esp_zb_scheduler_alarm(discovery_timeout_cb, 1, 5000);
        return;
    }

    // Simple descriptor timed out — try ZDO match_desc as fallback
    ESP_LOGW(TAG, "Simple descriptor timeout for %s - trying match descriptor as fallback", ieee_str);
    s_pending_discovery.match_desc_attempted = true;

    esp_zb_zdo_match_desc_req_param_t match_req = {
        .dst_nwk_addr = s_pending_discovery.short_addr,
        .addr_of_interest = s_pending_discovery.short_addr,
    };
    find_on_off_device(&match_req, user_find_on_off_cb, NULL);

    // Timeout in case match_desc response never arrives
    esp_zb_scheduler_alarm(discovery_timeout_cb, 1, 5000);
}

// Pending command tracking for retry
typedef struct {
    uint64_t ieee_addr;
    uint8_t endpoint;
    zigbee_cmd_type_t cmd;
    uint8_t retry_count;
    bool pending;
} pending_cmd_t;

static pending_cmd_t s_pending_cmd = {0};

// ============================================================================
// Zigbee Callbacks
// ============================================================================

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_LOGI(TAG, "BDB commissioning started, mode: 0x%02x", mode_mask);
    esp_zb_bdb_start_top_level_commissioning(mode_mask);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started, forming network");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory new device, start network formation");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ESP_LOGI(TAG, "Device rebooted, network already formed");
                s_zigbee_running = true;
                xEventGroupSetBits(g_event_group, EVENT_ZIGBEE_MODE_BIT);
            }
        } else {
            ESP_LOGW(TAG, "Device start failed, status: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Network formed, Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0]);
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            s_zigbee_running = true;
            xEventGroupSetBits(g_event_group, EVENT_ZIGBEE_MODE_BIT);
        } else {
            ESP_LOGW(TAG, "Network formation failed, retrying...");
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Network steering completed");
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        {
            esp_zb_zdo_signal_device_annce_params_t *dev_annce =
                (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);

            if (dev_annce == NULL) {
                ESP_LOGE(TAG, "Device announce params is NULL!");
                break;
            }

            ESP_LOGI(TAG, "New device joined: short_addr=0x%04x", dev_annce->device_short_addr);

            // Format and log IEEE address
            char ieee_str[24];
            format_ieee_addr(ieee_str, sizeof(ieee_str), dev_annce->ieee_addr);
            ESP_LOGI(TAG, "IEEE addr: %s", ieee_str);

            // Convert IEEE address to uint64_t using memcpy
            uint64_t ieee_addr = ieee_to_uint64(dev_annce->ieee_addr);

            // Debug: verify the conversion
            ESP_LOGI(TAG, "IEEE addr uint64 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                     (uint8_t)(ieee_addr & 0xFF),
                     (uint8_t)((ieee_addr >> 8) & 0xFF),
                     (uint8_t)((ieee_addr >> 16) & 0xFF),
                     (uint8_t)((ieee_addr >> 24) & 0xFF),
                     (uint8_t)((ieee_addr >> 32) & 0xFF),
                     (uint8_t)((ieee_addr >> 40) & 0xFF),
                     (uint8_t)((ieee_addr >> 48) & 0xFF),
                     (uint8_t)((ieee_addr >> 56) & 0xFF));

            // Validate IEEE address - must not be 0
            if (ieee_addr == 0) {
                ESP_LOGW(TAG, "Invalid IEEE address (0), skipping device");
                break;
            }

            // Check if device already exists
            if (device_manager_exists(ieee_addr)) {
                ESP_LOGI(TAG, "Device %s already registered", ieee_str);
                break;
            }

            // Start device discovery - request active endpoints to find all clusters
            s_pending_discovery.short_addr = dev_annce->device_short_addr;
            s_pending_discovery.ieee_addr = ieee_addr;
            s_pending_discovery.pending = true;
            s_pending_discovery.match_desc_attempted = false;
            s_pending_discovery.find_light_attempted = false;

            ESP_LOGI(TAG, "Starting device discovery for %s - requesting active endpoints", ieee_str);

            // Directly request active endpoints to discover all clusters (ON/OFF, temp, humidity, etc.)
            request_active_endpoints(dev_annce->device_short_addr);
        }
        break;

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        {
            uint8_t *permit_join_duration = (uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (*permit_join_duration) {
                ESP_LOGI(TAG, "Permit join enabled for %d seconds", *permit_join_duration);
            } else {
                ESP_LOGI(TAG, "Permit join disabled");
            }
        }
        break;

    default:
        ESP_LOGD(TAG, "Unhandled Zigbee signal: %d, status: %s",
                 sig_type, esp_err_to_name(err_status));
        break;
    }
}

// External declarations for sensor data queue
extern QueueHandle_t g_sensor_data_queue;

static esp_err_t zb_attribute_reporting_handler(esp_zb_zcl_report_attr_message_t *message)
{
    if (message == NULL) {
        ESP_LOGW(TAG, "Attribute report message is NULL");
        return ESP_FAIL;
    }

    esp_zb_zcl_report_attr_message_t *report = message;

    ESP_LOGD(TAG, "Attribute report: src=0x%04x ep=%d cluster=0x%04x",
             report->src_address.u.short_addr, report->src_endpoint, report->cluster);

    // Auto-detect ON/OFF device from attribute report — handles devices that
    // don't respond to simple_desc / match_desc (e.g. certain IKEA switches)
    if (report->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        esp_zb_lock_acquire(portMAX_DELAY);
        uint8_t ieee_bytes[8];
        esp_zb_ieee_address_by_short(report->src_address.u.short_addr, ieee_bytes);
        esp_zb_lock_release();

        uint64_t ieee_addr = ieee_to_uint64(ieee_bytes);

        // If IEEE address lookup failed, request it from the device
        if (ieee_addr == 0xFFFFFFFFFFFFFFFF || ieee_addr == 0) {
            request_ieee_for_short(report->src_address.u.short_addr);
            return ESP_OK;
        }

        esp_err_t ret = device_manager_add(ieee_addr, report->src_endpoint, NULL, NULL);
        if (ret == ESP_OK) {
            char ieee_str[24];
            format_ieee_addr_u64(ieee_str, sizeof(ieee_str), ieee_addr);
            ESP_LOGI(TAG, "Auto-detected ON/OFF device %s ep=%d from attribute report",
                     ieee_str, report->src_endpoint);
        }
        // ret == ESP_ERR_INVALID_STATE → already known, silent skip
        return ESP_OK;
    }

    // Handle Power Configuration cluster (battery reports) — cluster 0x0001
    if (report->cluster == 0x0001) {
        esp_zb_lock_acquire(portMAX_DELAY);
        uint8_t batt_ieee_bytes[8];
        esp_zb_ieee_address_by_short(report->src_address.u.short_addr, batt_ieee_bytes);
        esp_zb_lock_release();
        uint64_t batt_ieee_addr = ieee_to_uint64(batt_ieee_bytes);

        // If IEEE address lookup failed, request it from the device
        if (batt_ieee_addr == 0xFFFFFFFFFFFFFFFF || batt_ieee_addr == 0) {
            request_ieee_for_short(report->src_address.u.short_addr);
            return ESP_OK;
        }

        uint16_t batt_attr_id = report->attribute.id;
        uint8_t batt_raw = *(uint8_t *)report->attribute.data.value;

        if (batt_attr_id == 0x0021 && batt_raw != 0xFF) {
            // BatteryPercentageRemaining: uint8, half-percent (200 = 100%)
            uint8_t percent = batt_raw / 2;
            ESP_LOGI(TAG, "Battery %d%% (raw %d) from 0x%04x", percent, batt_raw, report->src_address.u.short_addr);
            device_manager_update_battery(batt_ieee_addr, percent, 0xFF);
        } else if (batt_attr_id == 0x0020 && batt_raw != 0xFF) {
            // BatteryVoltage: uint8, 100mV units
            ESP_LOGI(TAG, "Battery %.2fV (raw %d) from 0x%04x", batt_raw * 0.1f, batt_raw, report->src_address.u.short_addr);
            device_manager_update_battery(batt_ieee_addr, 0xFF, batt_raw);
        } else {
            ESP_LOGD(TAG, "Power config attr 0x%04x raw=%d, ignored", batt_attr_id, batt_raw);
        }
        return ESP_OK;
    }

    // Check if this is a temperature or humidity measurement
    if (report->cluster != ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT &&
        report->cluster != ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT) {
        ESP_LOGD(TAG, "Not a sensor cluster, ignoring");
        return ESP_OK;
    }

    // Get the attribute directly from the report message
    const esp_zb_zcl_attribute_t *attribute = &report->attribute;
    if (attribute == NULL) {
        ESP_LOGW(TAG, "Attribute is NULL");
        return ESP_FAIL;
    }

    // Parse attribute data
    uint16_t attr_id = attribute->id;
    uint8_t attr_type = attribute->data.type;
    void *attr_data = attribute->data.value;

    ESP_LOGI(TAG, "  Attribute ID: 0x%04x", attr_id);
    ESP_LOGI(TAG, "  Attribute type: 0x%02x", attr_type);

    // We're only interested in the measured value attribute (0x0000)
    // This is the same attribute ID for both temperature and humidity clusters
    if (attr_id != 0x0000) {
        ESP_LOGD(TAG, "Not a measurement value attribute, ignoring");
        return ESP_OK;
    }

    // Extract raw value
    int16_t raw_value = 0;
    if (attr_type == ESP_ZB_ZCL_ATTR_TYPE_S16) {
        // Temperature: INT16
        raw_value = *(int16_t *)attr_data;
    } else if (attr_type == ESP_ZB_ZCL_ATTR_TYPE_U16) {
        // Humidity: UINT16
        raw_value = (int16_t)(*(uint16_t *)attr_data);
    } else {
        ESP_LOGW(TAG, "Unexpected attribute type: 0x%02x", attr_type);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "  Raw value: %d", raw_value);

    // Convert short address to IEEE address
    esp_zb_lock_acquire(portMAX_DELAY);
    uint8_t ieee_bytes[8];
    esp_zb_ieee_address_by_short(report->src_address.u.short_addr, ieee_bytes);
    esp_zb_lock_release();

    uint64_t ieee_addr = ieee_to_uint64(ieee_bytes);

    // If IEEE address lookup failed, request it from the device via ZDO
    if (ieee_addr == 0xFFFFFFFFFFFFFFFF || ieee_addr == 0) {
        ESP_LOGW(TAG, "  IEEE address lookup failed for short 0x%04x - requesting via ZDO",
                 report->src_address.u.short_addr);
        request_ieee_for_short(report->src_address.u.short_addr);
        return ESP_OK;
    }

    char ieee_str[24];
    format_ieee_addr_u64(ieee_str, sizeof(ieee_str), ieee_addr);
    ESP_LOGI(TAG, "  IEEE address: %s", ieee_str);

    // Queue sensor data for processing by scheduler
    if (g_sensor_data_queue != NULL) {
        sensor_data_msg_t sensor_msg = {
            .ieee_addr = ieee_addr,
            .endpoint = report->src_endpoint,
            .cluster_id = report->cluster,
            .raw_value = raw_value,
            .timestamp = (uint32_t)time(NULL)
        };

        if (xQueueSend(g_sensor_data_queue, &sensor_msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "Sensor data queued for processing");
        } else {
            ESP_LOGW(TAG, "Failed to queue sensor data (queue full)");
        }
    } else {
        ESP_LOGW(TAG, "Sensor data queue not initialized");
    }

    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        {
            esp_zb_zcl_cmd_default_resp_message_t *resp =
                (esp_zb_zcl_cmd_default_resp_message_t *)message;
            ESP_LOGI(TAG, "Default response: status=%d", resp->info.status);

            if (s_pending_cmd.pending) {
                if (resp->info.status == ESP_ZB_ZCL_STATUS_SUCCESS) {
                    ESP_LOGI(TAG, "Command acknowledged successfully");
                    device_manager_clear_error(s_pending_cmd.ieee_addr);
                    s_pending_cmd.pending = false;
                } else {
                    ESP_LOGW(TAG, "Command failed with status %d", resp->info.status);
                    if (s_pending_cmd.retry_count < ZIGBEE_RETRY_COUNT) {
                        s_pending_cmd.retry_count++;
                        ESP_LOGI(TAG, "Retrying command (%d/%d)",
                                s_pending_cmd.retry_count, ZIGBEE_RETRY_COUNT);
                    } else {
                        device_manager_set_error(s_pending_cmd.ieee_addr, "Parancs sikertelen");
                        led_set_state(LED_STATE_ERROR);
                        s_pending_cmd.pending = false;
                    }
                }
            }
        }
        break;

    case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
        return zb_attribute_reporting_handler((esp_zb_zcl_report_attr_message_t *)message);

    default:
        ESP_LOGD(TAG, "Unhandled action callback: %d", callback_id);
        break;
    }

    return ESP_OK;
}

// ============================================================================
// Zigbee Task
// ============================================================================

static void zigbee_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Zigbee task started");

    // Initialize Zigbee stack
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();

    esp_zb_init(&zb_nwk_cfg);

    // Create On/Off Switch endpoint using HA standard configuration
    esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_switch_ep_create(HA_GATEWAY_ENDPOINT, &switch_cfg);

    // Add temperature measurement cluster (CLIENT ROLE to receive reports)
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, HA_GATEWAY_ENDPOINT);
    if (cluster_list != NULL) {
        esp_zb_attribute_list_t *temp_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
        esp_zb_cluster_list_add_temperature_meas_cluster(cluster_list, temp_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

        // Add humidity measurement cluster (CLIENT ROLE to receive reports)
        esp_zb_attribute_list_t *hum_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
        esp_zb_cluster_list_add_humidity_meas_cluster(cluster_list, hum_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

        ESP_LOGI(TAG, "Added temperature and humidity client clusters to coordinator");
    } else {
        ESP_LOGW(TAG, "Failed to get cluster list for endpoint");
    }

    // Register device
    esp_zb_device_register(ep_list);

    // Register action handler
    esp_zb_core_action_handler_register(zb_action_handler);

    // Set primary channel mask (channel 11-26)
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));

    // Main Zigbee loop (never returns)
    esp_zb_stack_main_loop();
}

// Internal helper to resend ZCL command without resetting pending state (for retries)
static void resend_zcl_on_off_cmd(uint64_t ieee_addr, uint8_t endpoint, uint8_t on_off_cmd_id)
{
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_ieee_addr_t ieee;
    for (int i = 0; i < 8; i++) {
        ieee[i] = (ieee_addr >> (i * 8)) & 0xFF;
    }

    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);
    if (short_addr == 0xFFFF) {
        ESP_LOGW(TAG, "Device not found in network for retry");
        esp_zb_lock_release();
        return;
    }

    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = on_off_cmd_id,
    };

    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void zigbee_cmd_processor_task(void *pvParameters)
{
    cmd_queue_msg_t msg;

    ESP_LOGI(TAG, "Zigbee command processor started");

    for (;;) {
        if (xQueueReceive(g_cmd_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // Check if Wi-Fi mode is active (should not send commands)
            EventBits_t bits = xEventGroupGetBits(g_event_group);
            if (bits & EVENT_WIFI_MODE_BIT) {
                ESP_LOGW(TAG, "Wi-Fi mode active, skipping command");
                continue;
            }

            char cmd_ieee_str[24];
            format_ieee_addr_u64(cmd_ieee_str, sizeof(cmd_ieee_str), msg.ieee_addr);
            const char *cmd_name = (msg.cmd == CMD_ON) ? "ON" :
                                   (msg.cmd == CMD_OFF) ? "OFF" : "TOGGLE";
            ESP_LOGI(TAG, "Processing %s command for device %s (ep=%d)",
                     cmd_name, cmd_ieee_str, msg.endpoint);

            esp_err_t send_result = ESP_OK;
            switch (msg.cmd) {
            case CMD_ON:
                send_result = zigbee_send_on(msg.ieee_addr, msg.endpoint);
                break;
            case CMD_OFF:
                send_result = zigbee_send_off(msg.ieee_addr, msg.endpoint);
                break;
            case CMD_TOGGLE:
                send_result = zigbee_send_toggle(msg.ieee_addr, msg.endpoint);
                break;
            }

            // If initial send failed (device not found), skip retries
            if (send_result != ESP_OK) {
                ESP_LOGW(TAG, "Initial send failed, skipping retries");
                continue;
            }

            // Wait for response or timeout
            vTaskDelay(pdMS_TO_TICKS(ZIGBEE_CMD_TIMEOUT_MS));

            // Check if retry is needed - but abort if new command arrives
            while (s_pending_cmd.pending && s_pending_cmd.retry_count < ZIGBEE_RETRY_COUNT) {
                // Check if there's a new command in queue - if yes, abort retry
                if (uxQueueMessagesWaiting(g_cmd_queue) > 0) {
                    ESP_LOGI(TAG, "New command in queue, aborting retry for current command");
                    s_pending_cmd.pending = false;
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(ZIGBEE_RETRY_DELAY_MS));

                // Check again after delay
                if (uxQueueMessagesWaiting(g_cmd_queue) > 0) {
                    ESP_LOGI(TAG, "New command in queue, aborting retry for current command");
                    s_pending_cmd.pending = false;
                    break;
                }

                s_pending_cmd.retry_count++;
                ESP_LOGI(TAG, "Retrying %s command (%d/%d)",
                         cmd_name, s_pending_cmd.retry_count, ZIGBEE_RETRY_COUNT);

                // Use internal resend function that doesn't reset pending state
                uint8_t zcl_cmd_id;
                switch (msg.cmd) {
                case CMD_ON:
                    zcl_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_ON_ID;
                    break;
                case CMD_OFF:
                    zcl_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
                    break;
                case CMD_TOGGLE:
                default:
                    zcl_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID;
                    break;
                }
                resend_zcl_on_off_cmd(msg.ieee_addr, msg.endpoint, zcl_cmd_id);

                vTaskDelay(pdMS_TO_TICKS(ZIGBEE_CMD_TIMEOUT_MS));
            }

            if (s_pending_cmd.pending) {
                ESP_LOGW(TAG, "Command failed after %d retries", ZIGBEE_RETRY_COUNT);
                device_manager_set_error(msg.ieee_addr, "Nem valaszol");
                led_set_state(LED_STATE_ERROR);
                s_pending_cmd.pending = false;
            }
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t zigbee_task_init(void)
{
    ESP_LOGI(TAG, "Initializing Zigbee task");

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };

    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    return ESP_OK;
}

esp_err_t zigbee_task_start(void)
{
    BaseType_t ret = xTaskCreate(
        zigbee_task,
        "zigbee_task",
        TASK_STACK_ZIGBEE,
        NULL,
        TASK_PRIORITY_ZIGBEE,
        &s_zigbee_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return ESP_FAIL;
    }

    // Create command processor task
    ret = xTaskCreate(
        zigbee_cmd_processor_task,
        "zb_cmd_proc",
        4096,
        NULL,
        TASK_PRIORITY_ZIGBEE - 1,
        NULL
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create command processor task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Zigbee task started");
    return ESP_OK;
}

esp_err_t zigbee_permit_join(uint8_t duration)
{
    if (!s_zigbee_running) {
        ESP_LOGW(TAG, "Zigbee not running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_bdb_open_network(duration);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "Permit join enabled for %d seconds", duration);
    return ESP_OK;
}

esp_err_t zigbee_send_on(uint64_t ieee_addr, uint8_t endpoint)
{
    if (!s_zigbee_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pending_cmd.ieee_addr = ieee_addr;
    s_pending_cmd.endpoint = endpoint;
    s_pending_cmd.cmd = CMD_ON;
    s_pending_cmd.retry_count = 0;
    s_pending_cmd.pending = true;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_ieee_addr_t ieee;
    for (int i = 0; i < 8; i++) {
        ieee[i] = (ieee_addr >> (i * 8)) & 0xFF;
    }

    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);
    char on_ieee_str[24];
    format_ieee_addr_u64(on_ieee_str, sizeof(on_ieee_str), ieee_addr);

    if (short_addr == 0xFFFF) {
        ESP_LOGW(TAG, "Device %s not found in network (short addr unknown)", on_ieee_str);
        s_pending_cmd.pending = false;
        esp_zb_lock_release();
        return ESP_ERR_NOT_FOUND;
    }

    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_ON_ID,
    };

    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "ON command sent to %s (short: 0x%04X)", on_ieee_str, short_addr);

    return ESP_OK;
}

esp_err_t zigbee_send_off(uint64_t ieee_addr, uint8_t endpoint)
{
    if (!s_zigbee_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pending_cmd.ieee_addr = ieee_addr;
    s_pending_cmd.endpoint = endpoint;
    s_pending_cmd.cmd = CMD_OFF;
    s_pending_cmd.retry_count = 0;
    s_pending_cmd.pending = true;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_ieee_addr_t ieee;
    for (int i = 0; i < 8; i++) {
        ieee[i] = (ieee_addr >> (i * 8)) & 0xFF;
    }

    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);
    char off_ieee_str[24];
    format_ieee_addr_u64(off_ieee_str, sizeof(off_ieee_str), ieee_addr);

    if (short_addr == 0xFFFF) {
        ESP_LOGW(TAG, "Device %s not found in network (short addr unknown)", off_ieee_str);
        s_pending_cmd.pending = false;
        esp_zb_lock_release();
        return ESP_ERR_NOT_FOUND;
    }

    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID,
    };

    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "OFF command sent to %s (short: 0x%04X)", off_ieee_str, short_addr);

    return ESP_OK;
}

esp_err_t zigbee_send_toggle(uint64_t ieee_addr, uint8_t endpoint)
{
    if (!s_zigbee_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pending_cmd.ieee_addr = ieee_addr;
    s_pending_cmd.endpoint = endpoint;
    s_pending_cmd.cmd = CMD_TOGGLE;
    s_pending_cmd.retry_count = 0;
    s_pending_cmd.pending = true;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_ieee_addr_t ieee;
    for (int i = 0; i < 8; i++) {
        ieee[i] = (ieee_addr >> (i * 8)) & 0xFF;
    }

    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);
    char toggle_ieee_str[24];
    format_ieee_addr_u64(toggle_ieee_str, sizeof(toggle_ieee_str), ieee_addr);

    if (short_addr == 0xFFFF) {
        ESP_LOGW(TAG, "Device %s not found in network (short addr unknown)", toggle_ieee_str);
        s_pending_cmd.pending = false;
        esp_zb_lock_release();
        return ESP_ERR_NOT_FOUND;
    }

    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID,
    };

    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "TOGGLE command sent to %s (short: 0x%04X)", toggle_ieee_str, short_addr);

    return ESP_OK;
}

bool zigbee_is_running(void)
{
    return s_zigbee_running;
}

esp_err_t zigbee_request_leave(uint64_t ieee_addr)
{
    if (!s_zigbee_running) {
        ESP_LOGW(TAG, "Zigbee not running, cannot send leave request");
        return ESP_ERR_INVALID_STATE;
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    // Convert uint64_t to esp_zb_ieee_addr_t
    esp_zb_ieee_addr_t ieee;
    uint64_to_ieee(ieee_addr, ieee);

    char ieee_str[24];
    format_ieee_addr(ieee_str, sizeof(ieee_str), ieee);
    ESP_LOGI(TAG, "Requesting device %s to leave the network", ieee_str);

    // Get short address for the device
    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);
    if (short_addr == 0xFFFF) {
        ESP_LOGW(TAG, "Device not found in network, may have already left");
        esp_zb_lock_release();
        return ESP_OK;  // Not an error - device is not in network
    }

    // Send management leave request
    esp_zb_zdo_mgmt_leave_req_param_t leave_req = {
        .dst_nwk_addr = short_addr,
        .rejoin = 0,       // Don't rejoin
        .remove_children = 1,  // Remove children too
    };
    memcpy(leave_req.device_address, ieee, sizeof(esp_zb_ieee_addr_t));

    esp_zb_zdo_device_leave_req(&leave_req, NULL, NULL);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "Leave request sent to device %s (short: 0x%04x)", ieee_str, short_addr);

    return ESP_OK;
}

// ============================================================================
// Device Discovery Functions
// ============================================================================

static void find_on_off_device(esp_zb_zdo_match_desc_req_param_t *param, esp_zb_zdo_match_desc_callback_t user_cb, void *user_ctx)
{
    uint16_t cluster_list[] = {ESP_ZB_ZCL_CLUSTER_ID_ON_OFF};
    param->profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    param->num_in_clusters = 1;
    param->num_out_clusters = 0;
    param->cluster_list = cluster_list;
    esp_zb_zdo_match_cluster(param, user_cb, user_ctx);
}

static void user_find_on_off_cb(esp_zb_zdp_status_t zdo_status, uint16_t peer_addr, uint8_t peer_endpoint, void *user_ctx)
{
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Found ON/OFF device at short_addr=0x%04x, endpoint=%d", peer_addr, peer_endpoint);

        if (!s_pending_discovery.pending) {
            ESP_LOGW(TAG, "No pending discovery - ignoring");
            return;
        }

        // Verify the short address matches our pending discovery
        if (peer_addr != s_pending_discovery.short_addr) {
            ESP_LOGW(TAG, "Short address mismatch (expected 0x%04x, got 0x%04x)",
                     s_pending_discovery.short_addr, peer_addr);
            return;
        }

        // Add the device with the discovered endpoint
        uint8_t ieee_bytes[8];
        uint64_to_ieee(s_pending_discovery.ieee_addr, ieee_bytes);
        char ieee_str[24];
        format_ieee_addr(ieee_str, sizeof(ieee_str), ieee_bytes);

        ESP_LOGI(TAG, "Adding ON/OFF device %s with endpoint %d", ieee_str, peer_endpoint);

        esp_err_t ret = device_manager_add(s_pending_discovery.ieee_addr, peer_endpoint, NULL, NULL);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Device added successfully");
        } else {
            ESP_LOGW(TAG, "Failed to add device: %s", esp_err_to_name(ret));
        }

        s_pending_discovery.pending = false;
        s_pending_discovery.expected_responses = 0;
    } else {
        ESP_LOGW(TAG, "ON/OFF device search failed with status %d", zdo_status);

        if (s_pending_discovery.pending) {
            if (!s_pending_discovery.match_desc_attempted) {
                // First attempt failed, fall back to endpoint discovery
                ESP_LOGI(TAG, "Falling back to basic endpoint discovery");
                request_active_endpoints(s_pending_discovery.short_addr);
            } else if (!s_pending_discovery.find_light_attempted) {
                // match_desc failed — try esp_zb_zdo_find_on_off_light (SDK convenience,
                // may handle edge cases that raw match_cluster misses)
                ESP_LOGI(TAG, "match_desc failed, trying esp_zb_zdo_find_on_off_light");
                s_pending_discovery.find_light_attempted = true;

                esp_zb_zdo_match_desc_req_param_t find_req = {
                    .dst_nwk_addr = s_pending_discovery.short_addr,
                    .addr_of_interest = s_pending_discovery.short_addr,
                };
                esp_zb_zdo_find_on_off_light(&find_req, user_find_on_off_cb, NULL);

                // Timeout in case find_on_off_light response never arrives
                esp_zb_scheduler_alarm(discovery_timeout_cb, 2, 5000);
            } else {
                // All ZDP methods exhausted — give up (attribute-report auto-detect may still catch it)
                ESP_LOGW(TAG, "All discovery methods failed - device not supported");
                s_pending_discovery.pending = false;
                s_pending_discovery.expected_responses = 0;
            }
        }
    }
}

static void request_active_endpoints(uint16_t short_addr)
{
    esp_zb_zdo_active_ep_req_param_t req = {
        .addr_of_interest = short_addr,
    };

    ESP_LOGI(TAG, "Requesting active endpoints from 0x%04x", short_addr);
    esp_zb_zdo_active_ep_req(&req, active_ep_cb, NULL);
}

static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_list, void *user_ctx)
{
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Active endpoint request failed: %d", zdo_status);
        s_pending_discovery.pending = false;
        return;
    }

    ESP_LOGI(TAG, "Device has %d endpoints", ep_count);

    if (ep_count == 0) {
        ESP_LOGW(TAG, "No endpoints found");
        s_pending_discovery.pending = false;
        return;
    }

    // Log all endpoints
    for (uint8_t i = 0; i < ep_count; i++) {
        ESP_LOGI(TAG, "  Endpoint %d: %d", i, ep_list[i]);
    }

    // Request simple descriptor for each endpoint to verify ON_OFF cluster
    if (s_pending_discovery.pending) {
        ESP_LOGI(TAG, "Checking endpoints for ON_OFF cluster support");

        s_pending_discovery.expected_responses = ep_count;

        for (uint8_t i = 0; i < ep_count; i++) {
            esp_zb_zdo_simple_desc_req_param_t req = {
                .addr_of_interest = s_pending_discovery.short_addr,
                .endpoint = ep_list[i],
            };

            ESP_LOGI(TAG, "Requesting simple descriptor for endpoint %d", ep_list[i]);
            esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, (void *)(uintptr_t)ep_list[i]);
        }

        // Set a timeout to clear pending state if no ON_OFF cluster is found
        esp_zb_scheduler_alarm(discovery_timeout_cb, 0, 5000);  // 5 second timeout
    }
}

/**
 * @brief Configure sensor reporting intervals
 * @param short_addr Short address of the sensor
 * @param endpoint Endpoint ID
 * @param cluster_id Cluster ID (temperature or humidity)
 */
static void configure_sensor_reporting(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id,
                                       uint16_t min_interval, uint16_t max_interval, int16_t report_change_val)
{
    // Fallback defaults for 0 values (e.g. old NVS data before these fields existed)
    if (max_interval == 0) max_interval = 10;
    if (min_interval > max_interval) min_interval = 0;
    int16_t report_change = (report_change_val != 0) ? report_change_val : 50;

    ESP_LOGI(TAG, "Configuring reporting for cluster 0x%04x on 0x%04x ep=%d (min=%d max=%d chg=%d)",
             cluster_id, short_addr, endpoint, min_interval, max_interval, report_change);

    esp_zb_zcl_config_report_cmd_t report_cmd = {0};
    report_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    report_cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    report_cmd.zcl_basic_cmd.dst_endpoint = endpoint;
    report_cmd.zcl_basic_cmd.src_endpoint = HA_GATEWAY_ENDPOINT;
    report_cmd.clusterID = cluster_id;

    esp_zb_zcl_config_report_record_t record = {
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .attributeID = 0x0000,  // Measured value attribute
        .attrType = (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT) ?
                    ESP_ZB_ZCL_ATTR_TYPE_S16 : ESP_ZB_ZCL_ATTR_TYPE_U16,
        .min_interval = min_interval,
        .max_interval = max_interval,
        .reportable_change = &report_change,
    };

    report_cmd.record_number = 1;
    report_cmd.record_field = &record;

    // Retry mechanism: Send command 5 times with 500ms intervals
    // This helps reach battery-powered sensors that may be sleeping
    const int max_retries = 5;
    int success_count = 0;

    for (int i = 0; i < max_retries; i++) {
        esp_zb_lock_acquire(portMAX_DELAY);
        esp_err_t ret = esp_zb_zcl_config_report_cmd_req(&report_cmd);
        esp_zb_lock_release();

        if (ret == ESP_OK) {
            success_count++;
            ESP_LOGI(TAG, "Configure reporting command sent (attempt %d/%d)", i + 1, max_retries);
        } else {
            ESP_LOGW(TAG, "Failed to send configure reporting command (attempt %d/%d): %s",
                     i + 1, max_retries, esp_err_to_name(ret));
        }

        // Wait 500ms before next retry (except after last attempt)
        if (i < max_retries - 1) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    if (success_count > 0) {
        ESP_LOGI(TAG, "Configure reporting completed: %d/%d attempts successful", success_count, max_retries);
    } else {
        ESP_LOGW(TAG, "Configure reporting failed: all %d attempts failed", max_retries);
    }
}

/**
 * @brief Configure battery reporting for Power Configuration cluster (0x0001).
 *        Configures both BatteryPercentageRemaining (0x0021) and BatteryVoltage (0x0020).
 *        Fixed intervals: max 10 min, 1% or 0.1V change triggers immediate report.
 */
static void configure_battery_reporting(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGI(TAG, "Configuring battery reporting on 0x%04x ep=%d (max_interval=600s)", short_addr, endpoint);

    // Configure BatteryPercentageRemaining (attr 0x0021, UINT8, half-percent units)
    // reportable_change=2 means 1% (since 1 unit = 0.5%)
    {
        uint8_t change_val = 2;
        esp_zb_zcl_config_report_record_t record = {
            .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
            .attributeID = 0x0021,
            .attrType = ESP_ZB_ZCL_ATTR_TYPE_U8,
            .min_interval = 0,
            .max_interval = 600,
            .reportable_change = &change_val,
        };

        esp_zb_zcl_config_report_cmd_t report_cmd = {0};
        report_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        report_cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
        report_cmd.zcl_basic_cmd.dst_endpoint = endpoint;
        report_cmd.zcl_basic_cmd.src_endpoint = HA_GATEWAY_ENDPOINT;
        report_cmd.clusterID = 0x0001;
        report_cmd.record_number = 1;
        report_cmd.record_field = &record;

        for (int i = 0; i < 3; i++) {
            esp_zb_lock_acquire(portMAX_DELAY);
            esp_err_t ret = esp_zb_zcl_config_report_cmd_req(&report_cmd);
            esp_zb_lock_release();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Battery percentage reporting configured (attempt %d)", i + 1);
            }
            if (i < 2) vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    // Configure BatteryVoltage (attr 0x0020, UINT8, 100mV units)
    // reportable_change=1 means 0.1V change
    {
        uint8_t change_val = 1;
        esp_zb_zcl_config_report_record_t record = {
            .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
            .attributeID = 0x0020,
            .attrType = ESP_ZB_ZCL_ATTR_TYPE_U8,
            .min_interval = 0,
            .max_interval = 600,
            .reportable_change = &change_val,
        };

        esp_zb_zcl_config_report_cmd_t report_cmd = {0};
        report_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        report_cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
        report_cmd.zcl_basic_cmd.dst_endpoint = endpoint;
        report_cmd.zcl_basic_cmd.src_endpoint = HA_GATEWAY_ENDPOINT;
        report_cmd.clusterID = 0x0001;
        report_cmd.record_number = 1;
        report_cmd.record_field = &record;

        for (int i = 0; i < 3; i++) {
            esp_zb_lock_acquire(portMAX_DELAY);
            esp_err_t ret = esp_zb_zcl_config_report_cmd_req(&report_cmd);
            esp_zb_lock_release();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Battery voltage reporting configured (attempt %d)", i + 1);
            }
            if (i < 2) vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

/* --------------- sensor bind (sensor → coordinator) --------------- */
typedef struct {
    esp_zb_zdo_bind_req_param_t bind_req;   /* embedded — SDK tartja a pointerét a callback-ig */
    uint16_t                    short_addr;
    uint8_t                     endpoint;
    uint16_t                    cluster_id;
    uint64_t                    ieee_addr;  /* sensor IEEE — for config lookup in callback */
} sensor_bind_ctx_t;

static void sensor_bind_cb(esp_zb_zdp_status_t status, void *user_ctx)
{
    sensor_bind_ctx_t *ctx = (sensor_bind_ctx_t *)user_ctx;

    if (status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Sensor bind SUCCESS: 0x%04x ep=%d cluster=0x%04x",
                 ctx->short_addr, ctx->endpoint, ctx->cluster_id);
    } else {
        ESP_LOGW(TAG, "Sensor bind FAILED (status=%d): 0x%04x ep=%d cluster=0x%04x — configure anyway",
                 status, ctx->short_addr, ctx->endpoint, ctx->cluster_id);
    }

    /* Configure reporting regardless of bind result */
    if (ctx->cluster_id == 0x0001) {
        /* Battery cluster — fixed intervals, no user config needed */
        configure_battery_reporting(ctx->short_addr, ctx->endpoint);
    } else {
        /* Temperature/humidity — use user-configurable intervals */
        uint16_t min_iv = 0, max_iv = 10;
        int16_t chg = 50;
        device_config_t dev;
        if (device_manager_get(ctx->ieee_addr, &dev) == ESP_OK) {
            min_iv = dev.sensor.report_min_interval;
            max_iv = dev.sensor.report_max_interval;
            chg    = dev.sensor.report_change;
        }
        configure_sensor_reporting(ctx->short_addr, ctx->endpoint, ctx->cluster_id, min_iv, max_iv, chg);
    }
    free(ctx);
}

/**
 * @brief Bind a sensor to the coordinator, then configure reporting.
 *        Bind tells the sensor: "report cluster X to coordinator endpoint Y".
 *        Called from simple_desc_cb — async, configure runs in sensor_bind_cb.
 */
static void bind_sensor(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id)
{
    sensor_bind_ctx_t *ctx = calloc(1, sizeof(sensor_bind_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "OOM sensor_bind_ctx — fallback direct configure");
        configure_sensor_reporting(short_addr, endpoint, cluster_id, 0, 10, 50);
        return;
    }
    ctx->short_addr = short_addr;
    ctx->endpoint   = endpoint;
    ctx->cluster_id = cluster_id;
    ctx->ieee_addr  = s_pending_discovery.ieee_addr;

    /* sensor IEEE from pending discovery (already resolved) */
    uint8_t sensor_ieee[8];
    uint64_to_ieee(s_pending_discovery.ieee_addr, sensor_ieee);

    /* bind_req: sensor → koordinátor
     *   src  = sensor (report forrás)
     *   dst  = koordinátor (report célja) */
    ctx->bind_req.req_dst_addr = short_addr;   /* a bind request-et a sensor-nak küldve */
    memcpy(ctx->bind_req.src_address, sensor_ieee, sizeof(esp_zb_ieee_addr_t));
    ctx->bind_req.src_endp      = endpoint;
    ctx->bind_req.cluster_id    = cluster_id;
    ctx->bind_req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
    esp_zb_get_long_address(ctx->bind_req.dst_address_u.addr_long);   /* koordinátor IEEE */
    ctx->bind_req.dst_endp      = HA_GATEWAY_ENDPOINT;

    ESP_LOGI(TAG, "Binding sensor 0x%04x ep=%d cluster=0x%04x → coordinator ep=%d",
             short_addr, endpoint, cluster_id, HA_GATEWAY_ENDPOINT);
    esp_zb_zdo_device_bind_req(&ctx->bind_req, sensor_bind_cb, ctx);
}

static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    uint8_t endpoint = (uint8_t)(uintptr_t)user_ctx;

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || simple_desc == NULL) {
        ESP_LOGW(TAG, "Simple descriptor request failed for endpoint %d: %d", endpoint, zdo_status);

        // Decrement expected responses counter even on failure
        if (s_pending_discovery.pending && s_pending_discovery.expected_responses > 0) {
            s_pending_discovery.expected_responses--;
            if (s_pending_discovery.expected_responses == 0) {
                // All simple descriptors failed/returned nothing — try match_desc fallback
                if (!s_pending_discovery.match_desc_attempted) {
                    ESP_LOGW(TAG, "All simple descriptors failed, trying match descriptor fallback");
                    s_pending_discovery.match_desc_attempted = true;

                    esp_zb_zdo_match_desc_req_param_t match_req = {
                        .dst_nwk_addr = s_pending_discovery.short_addr,
                        .addr_of_interest = s_pending_discovery.short_addr,
                    };
                    find_on_off_device(&match_req, user_find_on_off_cb, NULL);
                    // No alarm here — the existing param=0 alarm (from active_ep_cb)
                    // will fire shortly, see match_desc_attempted=true, and set
                    // a fresh param=1 alarm for 5s match_desc timeout.
                } else {
                    ESP_LOGW(TAG, "All simple descriptor requests completed, no supported device found");
                    s_pending_discovery.pending = false;
                }
            }
        }
        return;
    }

    ESP_LOGI(TAG, "Simple descriptor for endpoint %d:", endpoint);
    ESP_LOGI(TAG, "  Device ID: 0x%04x", simple_desc->app_device_id);
    ESP_LOGI(TAG, "  Profile ID: 0x%04x", simple_desc->app_profile_id);
    ESP_LOGI(TAG, "  Input clusters: %d", simple_desc->app_input_cluster_count);
    ESP_LOGI(TAG, "  Output clusters: %d", simple_desc->app_output_cluster_count);

    // Check if this is a Home Automation profile device
    if (simple_desc->app_profile_id != ESP_ZB_AF_HA_PROFILE_ID) {
        ESP_LOGD(TAG, "  Not a HA profile device, skipping");

        // Decrement expected responses counter
        if (s_pending_discovery.pending && s_pending_discovery.expected_responses > 0) {
            s_pending_discovery.expected_responses--;
            if (s_pending_discovery.expected_responses == 0) {
                ESP_LOGW(TAG, "All endpoints checked, no HA ON_OFF device found");
                s_pending_discovery.pending = false;
            }
        }
        return;
    }

    if (!s_pending_discovery.pending) {
        ESP_LOGD(TAG, "No pending discovery, ignoring simple descriptor");
        return;
    }

    // Check input clusters (server side) for ON_OFF, Temperature, and Humidity clusters
    bool has_on_off = false;
    bool has_temperature = false;
    bool has_humidity = false;

    for (int i = 0; i < simple_desc->app_input_cluster_count; i++) {
        uint16_t cluster_id = simple_desc->app_cluster_list[i];
        ESP_LOGD(TAG, "  Input cluster[%d]: 0x%04x", i, cluster_id);

        if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            has_on_off = true;
            ESP_LOGI(TAG, "  Found ON_OFF cluster on endpoint %d!", endpoint);
        } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT) {
            has_temperature = true;
            ESP_LOGI(TAG, "  Found TEMPERATURE_MEASUREMENT cluster on endpoint %d!", endpoint);
        } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT) {
            has_humidity = true;
            ESP_LOGI(TAG, "  Found HUMIDITY_MEASUREMENT cluster on endpoint %d!", endpoint);
        }
    }

    bool device_added = false;
    uint8_t ieee_bytes[8];
    uint64_to_ieee(s_pending_discovery.ieee_addr, ieee_bytes);
    char ieee_str[24];
    format_ieee_addr(ieee_str, sizeof(ieee_str), ieee_bytes);

    // Add temperature sensor
    if (has_temperature) {
        ESP_LOGI(TAG, "Adding TEMPERATURE sensor %s with endpoint %d", ieee_str, endpoint);
        esp_err_t ret = device_manager_add_sensor(s_pending_discovery.ieee_addr, endpoint,
                                                   DEVICE_TYPE_TEMPERATURE_SENSOR, NULL, NULL);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Temperature sensor added successfully");
            device_added = true;
            // Bind sensor → coordinator, then configure reporting in callback
            bind_sensor(s_pending_discovery.short_addr, endpoint,
                        ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "Temperature sensor already exists");
            device_added = true;  // Consider it added
        } else {
            ESP_LOGE(TAG, "Failed to add temperature sensor: %s", esp_err_to_name(ret));
        }
    }

    // Add humidity sensor (separate device entry, same IEEE, different endpoint or cluster)
    if (has_humidity) {
        ESP_LOGI(TAG, "Adding HUMIDITY sensor %s with endpoint %d", ieee_str, endpoint);
        esp_err_t ret = device_manager_add_sensor(s_pending_discovery.ieee_addr, endpoint,
                                                   DEVICE_TYPE_HUMIDITY_SENSOR, NULL, NULL);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Humidity sensor added successfully");
            device_added = true;
            // Bind sensor → coordinator, then configure reporting in callback
            bind_sensor(s_pending_discovery.short_addr, endpoint,
                        ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "Humidity sensor already exists");
            device_added = true;  // Consider it added
        } else {
            ESP_LOGE(TAG, "Failed to add humidity sensor: %s", esp_err_to_name(ret));
        }
    }

    // Bind battery cluster (0x0001) for any sensor device — configure periodic battery reporting
    if (has_temperature || has_humidity) {
        ESP_LOGI(TAG, "Binding battery cluster (0x0001) for sensor %s ep=%d", ieee_str, endpoint);
        bind_sensor(s_pending_discovery.short_addr, endpoint, 0x0001);
    }

    // Add ON/OFF device
    if (has_on_off) {
        ESP_LOGI(TAG, "Adding ON/OFF device %s with endpoint %d", ieee_str, endpoint);
        esp_err_t ret = device_manager_add(s_pending_discovery.ieee_addr, endpoint, NULL, NULL);
        if (ret == ESP_OK) {
            // Check device type for logging
            if (simple_desc->app_device_id == ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID) {
                ESP_LOGI(TAG, "Device type: ON_OFF_LIGHT");
            } else if (simple_desc->app_device_id == ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID) {
                ESP_LOGI(TAG, "Device type: ON_OFF_SWITCH");
            } else if (simple_desc->app_device_id == ESP_ZB_HA_DIMMABLE_LIGHT_DEVICE_ID) {
                ESP_LOGI(TAG, "Device type: DIMMABLE_LIGHT");
            } else {
                ESP_LOGI(TAG, "Device type: 0x%04x", simple_desc->app_device_id);
            }
            device_added = true;
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "ON/OFF device already exists");
            device_added = true;
        } else {
            ESP_LOGE(TAG, "Failed to add ON/OFF device: %s", esp_err_to_name(ret));
        }
    }

    if (device_added) {
        // Mark discovery as complete if we added at least one device type
        s_pending_discovery.pending = false;
        s_pending_discovery.expected_responses = 0;
    } else {
        ESP_LOGI(TAG, "Endpoint %d has no supported clusters (ON_OFF/TEMP/HUMIDITY), skipping", endpoint);

        // Decrement expected responses counter
        if (s_pending_discovery.expected_responses > 0) {
            s_pending_discovery.expected_responses--;
        }

        // If this was the last endpoint and no supported cluster found, clear pending
        if (s_pending_discovery.expected_responses == 0) {
            ESP_LOGW(TAG, "Device %s does not have any supported endpoints, not adding", ieee_str);
            s_pending_discovery.pending = false;
        }
    }
}

// Public function to reconfigure all sensors (called before BLE mode to wake sensors)
esp_err_t zigbee_reconfigure_all_sensors(void)
{
    if (!s_zigbee_running) {
        ESP_LOGW(TAG, "Zigbee not running, cannot reconfigure sensors");
        return ESP_ERR_INVALID_STATE;
    }

    device_config_t sensors[MAX_DEVICES];
    uint8_t sensor_count = MAX_DEVICES;

    esp_err_t ret = device_manager_get_sensors(sensors, &sensor_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get sensors: %s", esp_err_to_name(ret));
        return ret;
    }

    if (sensor_count == 0) {
        ESP_LOGI(TAG, "No sensors to reconfigure");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Reconfiguring %d sensor(s)", sensor_count);

    for (uint8_t i = 0; i < sensor_count; i++) {
        if (!sensors[i].enabled) {
            continue;
        }

        // Convert uint64_t IEEE address to esp_zb_ieee_addr_t (8-byte array)
        esp_zb_ieee_addr_t ieee;
        for (int j = 0; j < 8; j++) {
            ieee[j] = (sensors[i].ieee_addr >> (j * 8)) & 0xFF;
        }

        uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);
        if (short_addr == 0xFFFF) {
            ESP_LOGW(TAG, "Sensor %d: short address not found", i);
            continue;
        }

        uint16_t cluster_id;
        if (sensors[i].device_type == DEVICE_TYPE_TEMPERATURE_SENSOR) {
            cluster_id = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
        } else if (sensors[i].device_type == DEVICE_TYPE_HUMIDITY_SENSOR) {
            cluster_id = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
        } else {
            continue;  // Not a sensor
        }

        ESP_LOGI(TAG, "Reconfiguring sensor %d: type=%d, endpoint=%d",
                 i, sensors[i].device_type, sensors[i].endpoint);
        configure_sensor_reporting(short_addr, sensors[i].endpoint, cluster_id,
                                   sensors[i].sensor.report_min_interval,
                                   sensors[i].sensor.report_max_interval,
                                   sensors[i].sensor.report_change);
    }

    return ESP_OK;
}

// Public function to read all sensor data (called before BLE mode)
esp_err_t zigbee_read_all_sensor_data(void)
{
    if (!s_zigbee_running) {
        ESP_LOGW(TAG, "Zigbee not running, cannot read sensor data");
        return ESP_ERR_INVALID_STATE;
    }

    device_config_t sensors[MAX_DEVICES];
    uint8_t sensor_count = MAX_DEVICES;

    esp_err_t ret = device_manager_get_sensors(sensors, &sensor_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get sensors: %s", esp_err_to_name(ret));
        return ret;
    }

    if (sensor_count == 0) {
        ESP_LOGI(TAG, "No sensors to read");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Reading data from %d sensor(s)", sensor_count);

    for (uint8_t i = 0; i < sensor_count; i++) {
        if (!sensors[i].enabled) {
            continue;
        }

        // Convert uint64_t IEEE address to esp_zb_ieee_addr_t (8-byte array)
        esp_zb_ieee_addr_t ieee;
        for (int j = 0; j < 8; j++) {
            ieee[j] = (sensors[i].ieee_addr >> (j * 8)) & 0xFF;
        }

        uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);
        if (short_addr == 0xFFFF) {
            ESP_LOGW(TAG, "Sensor %d: short address not found", i);
            continue;
        }

        uint16_t cluster_id;
        if (sensors[i].device_type == DEVICE_TYPE_TEMPERATURE_SENSOR) {
            cluster_id = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
        } else if (sensors[i].device_type == DEVICE_TYPE_HUMIDITY_SENSOR) {
            cluster_id = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
        } else {
            continue;  // Not a sensor
        }

        ESP_LOGI(TAG, "Reading sensor %d: type=%d, endpoint=%d",
                 i, sensors[i].device_type, sensors[i].endpoint);

        // Send ZCL read attribute command
        esp_zb_zcl_read_attr_cmd_t read_cmd = {0};
        read_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        read_cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
        read_cmd.zcl_basic_cmd.dst_endpoint = sensors[i].endpoint;
        read_cmd.zcl_basic_cmd.src_endpoint = HA_GATEWAY_ENDPOINT;
        read_cmd.clusterID = cluster_id;

        uint16_t attr_id = 0x0000;  // Measured value attribute
        read_cmd.attr_number = 1;
        read_cmd.attr_field = &attr_id;

        esp_zb_lock_acquire(portMAX_DELAY);
        esp_err_t read_ret = esp_zb_zcl_read_attr_cmd_req(&read_cmd);
        esp_zb_lock_release();

        if (read_ret == ESP_OK) {
            ESP_LOGI(TAG, "Read attribute command sent to sensor %d", i);
        } else {
            ESP_LOGW(TAG, "Failed to send read attribute command to sensor %d: %s",
                     i, esp_err_to_name(read_ret));
        }

        // Small delay between read commands
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return ESP_OK;
}
