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
#include <string.h>

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
} pending_discovery_t;

static pending_discovery_t s_pending_discovery = {0};

// Forward declarations for device discovery
static void request_active_endpoints(uint16_t short_addr);
static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_list, void *user_ctx);
static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx);

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
            ESP_LOGI(TAG, "New device joined: short_addr=0x%04x",
                     dev_annce->device_short_addr);

            // Convert IEEE address to uint64_t
            uint64_t ieee_addr = 0;
            for (int i = 0; i < 8; i++) {
                ieee_addr |= ((uint64_t)dev_annce->ieee_addr[i]) << (i * 8);
            }

            // Check if device already exists
            if (device_manager_exists(ieee_addr)) {
                ESP_LOGI(TAG, "Device 0x%016llX already registered", (unsigned long long)ieee_addr);
                break;
            }

            // Start device discovery to find the correct endpoint with ON_OFF cluster
            s_pending_discovery.short_addr = dev_annce->device_short_addr;
            s_pending_discovery.ieee_addr = ieee_addr;
            s_pending_discovery.pending = true;

            ESP_LOGI(TAG, "Starting device discovery for 0x%016llX", (unsigned long long)ieee_addr);
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
                    device_manager_set_state(s_pending_cmd.ieee_addr,
                                           s_pending_cmd.cmd == CMD_ON);
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

            ESP_LOGI(TAG, "Processing command: addr=0x%016llX, ep=%d, cmd=%d",
                     (unsigned long long)msg.ieee_addr, msg.endpoint, msg.cmd);

            switch (msg.cmd) {
            case CMD_ON:
                zigbee_send_on(msg.ieee_addr, msg.endpoint);
                break;
            case CMD_OFF:
                zigbee_send_off(msg.ieee_addr, msg.endpoint);
                break;
            case CMD_TOGGLE:
                zigbee_send_toggle(msg.ieee_addr, msg.endpoint);
                break;
            }

            // Wait for response or timeout
            vTaskDelay(pdMS_TO_TICKS(ZIGBEE_CMD_TIMEOUT_MS));

            // Check if retry is needed
            while (s_pending_cmd.pending && s_pending_cmd.retry_count < ZIGBEE_RETRY_COUNT) {
                vTaskDelay(pdMS_TO_TICKS(ZIGBEE_RETRY_DELAY_MS));

                switch (msg.cmd) {
                case CMD_ON:
                    zigbee_send_on(msg.ieee_addr, msg.endpoint);
                    break;
                case CMD_OFF:
                    zigbee_send_off(msg.ieee_addr, msg.endpoint);
                    break;
                case CMD_TOGGLE:
                    zigbee_send_toggle(msg.ieee_addr, msg.endpoint);
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(ZIGBEE_CMD_TIMEOUT_MS));
            }

            if (s_pending_cmd.pending) {
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

    esp_zb_bdb_open_network(duration);
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

    esp_zb_ieee_addr_t ieee;
    for (int i = 0; i < 8; i++) {
        ieee[i] = (ieee_addr >> (i * 8)) & 0xFF;
    }

    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);

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
    ESP_LOGI(TAG, "ON command sent to 0x%016llX", (unsigned long long)ieee_addr);

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

    esp_zb_ieee_addr_t ieee;
    for (int i = 0; i < 8; i++) {
        ieee[i] = (ieee_addr >> (i * 8)) & 0xFF;
    }

    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);

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
    ESP_LOGI(TAG, "OFF command sent to 0x%016llX", (unsigned long long)ieee_addr);

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

    esp_zb_ieee_addr_t ieee;
    for (int i = 0; i < 8; i++) {
        ieee[i] = (ieee_addr >> (i * 8)) & 0xFF;
    }

    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee);

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
    ESP_LOGI(TAG, "TOGGLE command sent to 0x%016llX", (unsigned long long)ieee_addr);

    return ESP_OK;
}

bool zigbee_is_running(void)
{
    return s_zigbee_running;
}

// ============================================================================
// Device Discovery Functions
// ============================================================================

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

    // Request simple descriptor for each endpoint to find ON_OFF cluster
    for (uint8_t i = 0; i < ep_count; i++) {
        ESP_LOGI(TAG, "  Endpoint %d: %d", i, ep_list[i]);

        esp_zb_zdo_simple_desc_req_param_t desc_req = {
            .addr_of_interest = s_pending_discovery.short_addr,
            .endpoint = ep_list[i],
        };

        esp_zb_zdo_simple_desc_req(&desc_req, simple_desc_cb, (void *)(uintptr_t)ep_list[i]);
    }
}

static void simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    uint8_t endpoint = (uint8_t)(uintptr_t)user_ctx;

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || simple_desc == NULL) {
        ESP_LOGW(TAG, "Simple descriptor request failed for endpoint %d: %d", endpoint, zdo_status);
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
        return;
    }

    // Check input clusters (server side) for ON_OFF cluster
    bool has_on_off = false;
    for (int i = 0; i < simple_desc->app_input_cluster_count; i++) {
        uint16_t cluster_id = simple_desc->app_cluster_list[i];
        ESP_LOGD(TAG, "  Input cluster[%d]: 0x%04x", i, cluster_id);
        if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            has_on_off = true;
            ESP_LOGI(TAG, "  Found ON_OFF cluster on endpoint %d!", endpoint);
            break;
        }
    }

    if (has_on_off && s_pending_discovery.pending) {
        // Found an endpoint with ON_OFF cluster - add device
        ESP_LOGI(TAG, "Adding device 0x%016llX with endpoint %d",
                 (unsigned long long)s_pending_discovery.ieee_addr, endpoint);

        device_manager_add(s_pending_discovery.ieee_addr, endpoint, NULL, NULL);
        s_pending_discovery.pending = false;

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
    }
}
