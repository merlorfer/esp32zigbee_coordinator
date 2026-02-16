/**
 * @file main.c
 * @brief XKC-Y26-V Water Level Zigbee End Device
 *
 * Two XKC-Y26-V non-contact liquid level sensors provide 3 discrete levels:
 *   0 = empty (neither sensor detects water)
 *   1 = low   (lower sensor detects water)
 *   2 = full  (both sensors detect water)
 *
 * Uses Zigbee Pressure Measurement cluster (0x0403) MeasuredValue attribute.
 * Reports level changes to the coordinator.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

static const char *TAG = "XKC_LEVEL";

/* ============================================================================
 * Hardware Configuration
 * ============================================================================ */

#define GPIO_XKC_LOWER      GPIO_NUM_4      // Lower water level sensor
#define GPIO_XKC_UPPER      GPIO_NUM_5      // Upper water level sensor
#define GPIO_BUTTON         GPIO_NUM_9      // Boot button for pairing / manual report
#define GPIO_LED            GPIO_NUM_15     // Status LED

#define SENSOR_READ_INTERVAL_MS     500     // Read sensors every 500ms
#define BUTTON_LONG_PRESS_MS        3000    // 3 sec for pairing mode
#define BUTTON_DEBOUNCE_MS          50

/* ============================================================================
 * Zigbee Configuration
 * ============================================================================ */

#define XKC_ENDPOINT        1
#define MANUFACTURER_NAME   "DIY"
#define MODEL_IDENTIFIER    "XKC_WaterLevel_v1"

/* Zigbee End Device configuration */
#define ESP_ZB_ZED_CONFIG()                                         \
    {                                                               \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,                       \
        .install_code_policy = false,                               \
        .nwk_cfg.zed_cfg = {                                        \
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,            \
            .keep_alive = 3000,                                     \
        },                                                          \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()                               \
    {                                                               \
        .radio_mode = ZB_RADIO_MODE_NATIVE,                         \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                                \
    {                                                               \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,       \
    }

/* ============================================================================
 * LED States
 * ============================================================================ */

typedef enum {
    LED_OFF,            // No network
    LED_SLOW_BLINK,     // Connected to network
    LED_FAST_BLINK,     // Pairing in progress
} led_mode_t;

static volatile led_mode_t s_led_mode = LED_OFF;
static volatile bool s_connected = false;
static volatile bool s_has_network = false;  // True if NVS has stored network info

/* ============================================================================
 * LED Task
 * ============================================================================ */

static void led_task(void *pvParameters)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << GPIO_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(GPIO_LED, 0);

    for (;;) {
        switch (s_led_mode) {
        case LED_OFF:
            gpio_set_level(GPIO_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        case LED_SLOW_BLINK:
            gpio_set_level(GPIO_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            gpio_set_level(GPIO_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case LED_FAST_BLINK:
            gpio_set_level(GPIO_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            gpio_set_level(GPIO_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
            break;
        }
    }
}

/* ============================================================================
 * Sensor Reading
 * ============================================================================ */

static void sensor_gpio_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << GPIO_XKC_LOWER) | (1ULL << GPIO_XKC_UPPER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
}

static int16_t read_water_level(void)
{
    int lower = gpio_get_level(GPIO_XKC_LOWER);
    int upper = gpio_get_level(GPIO_XKC_UPPER);
    return (int16_t)(lower + upper);
}

/* ============================================================================
 * Zigbee Report Sending
 * ============================================================================ */

static void send_report(void)
{
    esp_zb_zcl_report_attr_cmd_t report_cmd = {0};
    report_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
    report_cmd.zcl_basic_cmd.src_endpoint = XKC_ENDPOINT;
    report_cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT;
    report_cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    report_cmd.attributeID = ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_report_attr_cmd_req(&report_cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "Report sent");
}

/* ============================================================================
 * Sensor Task - reads GPIOs and reports on change
 * ============================================================================ */

static void sensor_task(void *pvParameters)
{
    sensor_gpio_init();

    int16_t last_level = -1;  // Force first report

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));

        if (!s_connected) {
            continue;
        }

        int16_t level = read_water_level();

        if (level != last_level) {
            ESP_LOGI(TAG, "Water level changed: %d -> %d", last_level, level);
            last_level = level;

            // Update the attribute value
            esp_zb_lock_acquire(portMAX_DELAY);
            esp_zb_zcl_set_attribute_val(
                XKC_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID,
                &level,
                false);
            esp_zb_lock_release();

            // Send report
            send_report();
        }
    }
}

/* ============================================================================
 * Button Task - short press = report, long press = pairing
 * ============================================================================ */

static void button_task(void *pvParameters)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << GPIO_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));

        // Button is active LOW (pulled up, pressed = 0)
        if (gpio_get_level(GPIO_BUTTON) == 0) {
            // Button pressed — measure hold duration
            uint32_t press_start = xTaskGetTickCount();
            bool long_press = false;

            // Wait for release or long-press threshold
            while (gpio_get_level(GPIO_BUTTON) == 0) {
                vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
                uint32_t held_ms = (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;
                if (held_ms >= BUTTON_LONG_PRESS_MS) {
                    long_press = true;
                    break;
                }
            }

            if (long_press) {
                // Long press: start pairing (network steering)
                ESP_LOGI(TAG, "Long press detected - starting network steering");
                s_led_mode = LED_FAST_BLINK;

                esp_zb_lock_acquire(portMAX_DELAY);
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                esp_zb_lock_release();

                // Wait for button release
                while (gpio_get_level(GPIO_BUTTON) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            } else {
                // Short press: immediate report
                if (s_connected) {
                    ESP_LOGI(TAG, "Short press - sending immediate report");
                    send_report();
                } else {
                    ESP_LOGW(TAG, "Short press - not connected, ignoring");
                }
            }

            // Debounce after release
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* ============================================================================
 * Zigbee Signal Handler
 * ============================================================================ */

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
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "First start - waiting for button press to pair");
            // Do NOT auto-start steering on first boot
            s_has_network = false;
            s_led_mode = LED_OFF;
        } else {
            ESP_LOGW(TAG, "First start failed: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Reboot - rejoining previous network");
            s_has_network = true;
            s_connected = true;
            s_led_mode = LED_SLOW_BLINK;
        } else {
            ESP_LOGW(TAG, "Reboot rejoin failed: %s - press button to re-pair",
                     esp_err_to_name(err_status));
            s_connected = false;
            s_led_mode = LED_OFF;
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network, PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0]);
            s_connected = true;
            s_has_network = true;
            s_led_mode = LED_SLOW_BLINK;
        } else {
            ESP_LOGW(TAG, "Network steering failed: %s", esp_err_to_name(err_status));
            s_connected = false;
            s_led_mode = LED_OFF;
        }
        break;

    default:
        ESP_LOGD(TAG, "Zigbee signal: %d, status: %s", sig_type, esp_err_to_name(err_status));
        break;
    }
}

/* ============================================================================
 * Zigbee Endpoint Creation
 * ============================================================================ */

static void create_pressure_endpoint(void)
{
    /* --- Basic cluster attributes --- */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);

    /* Add manufacturer name */
    static char manufacturer[] = MANUFACTURER_NAME;
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                  manufacturer);

    /* Add model identifier */
    static char model[] = MODEL_IDENTIFIER;
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                  model);

    /* --- Identify cluster --- */
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = 0,
    };
    esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(&identify_cfg);

    /* --- Pressure Measurement cluster (server) --- */
    esp_zb_pressure_meas_cluster_cfg_t press_cfg = {
        .measured_value = 0,
        .min_value = 0,
        .max_value = 2,
    };
    esp_zb_attribute_list_t *press_cluster = esp_zb_pressure_meas_cluster_create(&press_cfg);

    /* --- Build cluster list --- */
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_pressure_meas_cluster(cluster_list, press_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* --- Build endpoint --- */
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = XKC_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);

    esp_zb_device_register(ep_list);
    ESP_LOGI(TAG, "Pressure measurement endpoint registered (ep=%d)", XKC_ENDPOINT);
}

/* ============================================================================
 * Zigbee Task
 * ============================================================================ */

static void zigbee_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Zigbee task started");

    esp_zb_cfg_t zb_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_cfg);

    create_pressure_endpoint();

    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));

    esp_zb_stack_main_loop();
}

/* ============================================================================
 * App Main
 * ============================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "XKC Water Level Sensor - Zigbee End Device");
    ESP_LOGI(TAG, "Lower sensor: GPIO%d, Upper sensor: GPIO%d", GPIO_XKC_LOWER, GPIO_XKC_UPPER);

    // Initialize NVS (required for Zigbee network persistence)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Zigbee platform
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    // Start LED task
    xTaskCreate(led_task, "led_task", 2048, NULL, 3, NULL);

    // Start button task
    xTaskCreate(button_task, "button_task", 2048, NULL, 12, NULL);

    // Start sensor reading task
    xTaskCreate(sensor_task, "sensor_task", 3072, NULL, 8, NULL);

    // Start Zigbee task (must run on main core for ESP-IDF Zigbee)
    xTaskCreate(zigbee_task, "zigbee_task", 4096, NULL, 10, NULL);
}
