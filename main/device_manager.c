/**
 * @file device_manager.c
 * @brief Device list management implementation
 */

#include "device_manager.h"
#include "sensor_types.h"
#include "nvs_manager.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "DEVICE_MANAGER";

/* Helper function to format IEEE address as hex string */
static void format_ieee_addr_str(char *buf, size_t buf_size, uint64_t ieee_addr)
{
    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)((ieee_addr >> 56) & 0xFF),
             (uint8_t)((ieee_addr >> 48) & 0xFF),
             (uint8_t)((ieee_addr >> 40) & 0xFF),
             (uint8_t)((ieee_addr >> 32) & 0xFF),
             (uint8_t)((ieee_addr >> 24) & 0xFF),
             (uint8_t)((ieee_addr >> 16) & 0xFF),
             (uint8_t)((ieee_addr >> 8) & 0xFF),
             (uint8_t)(ieee_addr & 0xFF));
}

static device_config_t s_devices[MAX_DEVICES];
static device_error_t s_errors[MAX_DEVICES];
static global_config_t s_global_config;
static uint8_t s_device_count = 0;

// External queue for sending commands to Zigbee task
extern QueueHandle_t g_cmd_queue;
extern SemaphoreHandle_t g_device_mutex;

esp_err_t device_manager_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_errors, 0, sizeof(s_errors));
    memset(&s_global_config, 0, sizeof(s_global_config));

    // Load global config
    esp_err_t ret = nvs_load_global_config(&s_global_config);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load global config: %s", esp_err_to_name(ret));
    }

    // Load device count
    ret = nvs_load_device_count(&s_device_count);
    if (ret != ESP_OK) {
        s_device_count = 0;
    }

    // Load devices
    for (uint8_t i = 0; i < s_device_count; i++) {
        ret = nvs_load_device(i, &s_devices[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load device %d", i);
            memset(&s_devices[i], 0, sizeof(device_config_t));
        }

        // Load associated error
        nvs_load_error(i, &s_errors[i]);
    }

    ESP_LOGI(TAG, "Device manager initialized, %d devices loaded", s_device_count);
    return ESP_OK;
}

esp_err_t device_manager_add(uint64_t ieee_addr, uint8_t endpoint,
                             const char *manufacturer, const char *model)
{
    if (s_device_count >= MAX_DEVICES) {
        ESP_LOGE(TAG, "Device limit reached (%d)", MAX_DEVICES);
        return ESP_ERR_NO_MEM;
    }

    // Check if (ieee_addr, endpoint) pair already exists
    for (uint8_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].ieee_addr == ieee_addr && s_devices[i].endpoint == endpoint) {
            char ieee_str[24];
            format_ieee_addr_str(ieee_str, sizeof(ieee_str), ieee_addr);
            ESP_LOGD(TAG, "Device %s ep=%d already exists", ieee_str, endpoint);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    device_config_t *dev = &s_devices[s_device_count];
    memset(dev, 0, sizeof(device_config_t));

    dev->ieee_addr = ieee_addr;
    dev->endpoint = endpoint;
    dev->device_type = DEVICE_TYPE_ON_OFF_LIGHT;  // Default type
    dev->enabled = false;  // Default: automation disabled
    dev->mode = MODE_FIXED_TIME;
    dev->time_pair_count = 1;
    dev->time_pairs[0].on_time.hour = 6;
    dev->time_pairs[0].on_time.minute = 0;
    dev->time_pairs[0].off_time.hour = 18;
    dev->time_pairs[0].off_time.minute = 0;

    // 3-phase delay defaults: OFF1 → ON → OFF2
    dev->delay_off1_minutes = 0;         // Start immediately (can be changed for sync)
    dev->delay_duration_minutes = 120;   // ON for 2 hours
    dev->delay_off2_minutes = 30;        // OFF for 30 minutes

    if (manufacturer != NULL) {
        strncpy(dev->manufacturer, manufacturer, MAX_MANUFACTURER_LEN - 1);
    }
    if (model != NULL) {
        strncpy(dev->model, model, MAX_MODEL_LEN - 1);
    }

    int ieee_first_idx = device_manager_find_index(ieee_addr);
    if (ieee_first_idx >= 0) {
        // Multi-endpoint device: name all entries for this IEEE with last-2-bytes + endpoint
        uint8_t b1 = (ieee_addr >> 8) & 0xFF;
        uint8_t b2 = ieee_addr & 0xFF;
        for (uint8_t i = 0; i < s_device_count; i++) {
            if (s_devices[i].ieee_addr == ieee_addr) {
                snprintf(s_devices[i].custom_name, MAX_DEVICE_NAME_LEN,
                         "%02X%02X_EP%d", b1, b2, s_devices[i].endpoint);
                nvs_save_device(i, &s_devices[i]);
            }
        }
        snprintf(dev->custom_name, MAX_DEVICE_NAME_LEN, "%02X%02X_EP%d", b1, b2, endpoint);
    } else {
        snprintf(dev->custom_name, MAX_DEVICE_NAME_LEN, "Device%d", s_device_count + 1);
    }

    s_device_count++;
    s_global_config.device_count = s_device_count;

    // Save to NVS
    nvs_save_device(s_device_count - 1, dev);
    nvs_save_device_count(s_device_count);
    nvs_save_global_config(&s_global_config);

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    char ieee_str[24];
    format_ieee_addr_str(ieee_str, sizeof(ieee_str), ieee_addr);
    ESP_LOGI(TAG, "Added device %s at index %d", ieee_str, s_device_count - 1);
    return ESP_OK;
}

esp_err_t device_manager_remove(uint64_t ieee_addr)
{
    if (device_manager_find_index(ieee_addr) < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    // Remove ALL entries with this IEEE address (multi-endpoint devices register multiple entries)
    uint8_t removed = 0;
    int i = 0;
    while (i < s_device_count) {
        if (s_devices[i].ieee_addr == ieee_addr) {
            // Shift remaining devices down
            for (int j = i; j < s_device_count - 1; j++) {
                memcpy(&s_devices[j], &s_devices[j + 1], sizeof(device_config_t));
                memcpy(&s_errors[j], &s_errors[j + 1], sizeof(device_error_t));
            }
            memset(&s_devices[s_device_count - 1], 0, sizeof(device_config_t));
            memset(&s_errors[s_device_count - 1], 0, sizeof(device_error_t));
            s_device_count--;
            removed++;
            // Don't increment i — recheck same index after shift
        } else {
            i++;
        }
    }

    s_global_config.device_count = s_device_count;

    // Save all devices to NVS (indices changed)
    for (uint8_t k = 0; k < s_device_count; k++) {
        nvs_save_device(k, &s_devices[k]);
        nvs_save_error(k, &s_errors[k]);
    }
    // Delete NVS slots that are now beyond the new count
    for (uint8_t k = s_device_count; k < s_device_count + removed; k++) {
        nvs_delete_device(k);
    }
    nvs_save_device_count(s_device_count);
    nvs_save_global_config(&s_global_config);

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    ESP_LOGI(TAG, "Removed %d device(s) with IEEE addr, %d devices remaining",
             removed, s_device_count);
    return ESP_OK;
}

esp_err_t device_manager_get(uint64_t ieee_addr, device_config_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = device_manager_find_index(ieee_addr);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(device, &s_devices[index], sizeof(device_config_t));
    return ESP_OK;
}

esp_err_t device_manager_get_by_index(uint8_t index, device_config_t *device)
{
    if (device == NULL || index >= s_device_count) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(device, &s_devices[index], sizeof(device_config_t));
    return ESP_OK;
}

esp_err_t device_manager_update(uint64_t ieee_addr, const device_config_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = device_manager_find_index(ieee_addr);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    memcpy(&s_devices[index], device, sizeof(device_config_t));
    nvs_save_device(index, &s_devices[index]);

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    char ieee_str[24];
    format_ieee_addr_str(ieee_str, sizeof(ieee_str), ieee_addr);
    ESP_LOGI(TAG, "Updated device %s", ieee_str);
    return ESP_OK;
}

uint8_t device_manager_get_count(void)
{
    return s_device_count;
}

int device_manager_find_index(uint64_t ieee_addr)
{
    for (uint8_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].ieee_addr == ieee_addr) {
            return i;
        }
    }
    return -1;
}

bool device_manager_exists(uint64_t ieee_addr)
{
    return device_manager_find_index(ieee_addr) >= 0;
}

int device_manager_find_index_by_type(uint64_t ieee_addr, device_type_t device_type)
{
    for (uint8_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].ieee_addr == ieee_addr && s_devices[i].device_type == device_type) {
            return i;
        }
    }
    return -1;
}

esp_err_t device_manager_get_by_type(uint64_t ieee_addr, device_type_t device_type, device_config_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int index = device_manager_find_index_by_type(ieee_addr, device_type);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(device, &s_devices[index], sizeof(device_config_t));
    return ESP_OK;
}

esp_err_t device_manager_update_by_type(uint64_t ieee_addr, device_type_t device_type, const device_config_t *device)
{
    int index = device_manager_find_index_by_type(ieee_addr, device_type);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    memcpy(&s_devices[index], device, sizeof(device_config_t));
    nvs_save_device(index, &s_devices[index]);

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    return ESP_OK;
}

esp_err_t device_manager_set_error(uint64_t ieee_addr, const char *error_message)
{
    int index = device_manager_find_index(ieee_addr);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    s_errors[index].ieee_addr = ieee_addr;
    s_errors[index].timestamp = (uint32_t)time(NULL);
    s_errors[index].active = true;
    if (error_message != NULL) {
        strncpy(s_errors[index].error_message, error_message, MAX_ERROR_MSG_LEN - 1);
    }

    nvs_save_error(index, &s_errors[index]);

    char ieee_str[24];
    format_ieee_addr_str(ieee_str, sizeof(ieee_str), ieee_addr);
    ESP_LOGW(TAG, "Error set for device %s: %s", ieee_str, error_message ? error_message : "unknown");
    return ESP_OK;
}

esp_err_t device_manager_clear_error(uint64_t ieee_addr)
{
    int index = device_manager_find_index(ieee_addr);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(&s_errors[index], 0, sizeof(device_error_t));
    nvs_save_error(index, &s_errors[index]);

    return ESP_OK;
}

esp_err_t device_manager_get_error(uint64_t ieee_addr, device_error_t *error)
{
    if (error == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = device_manager_find_index(ieee_addr);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_errors[index].active) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(error, &s_errors[index], sizeof(device_error_t));
    return ESP_OK;
}

esp_err_t device_manager_clear_all_errors(void)
{
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        memset(&s_errors[i], 0, sizeof(device_error_t));
    }

    nvs_clear_all_errors();
    ESP_LOGI(TAG, "All errors cleared");
    return ESP_OK;
}

esp_err_t device_manager_save_all(void)
{
    esp_err_t ret = ESP_OK;

    for (uint8_t i = 0; i < s_device_count; i++) {
        esp_err_t r = nvs_save_device(i, &s_devices[i]);
        if (r != ESP_OK) {
            ret = r;
        }
    }

    nvs_save_device_count(s_device_count);
    nvs_save_global_config(&s_global_config);

    ESP_LOGI(TAG, "All devices saved to NVS");
    return ret;
}

void device_manager_power_off_all(void)
{
    if (g_cmd_queue == NULL) {
        ESP_LOGW(TAG, "Command queue not available");
        return;
    }

    for (uint8_t i = 0; i < s_device_count; i++) {
        cmd_queue_msg_t msg = {
            .ieee_addr = s_devices[i].ieee_addr,
            .endpoint = s_devices[i].endpoint,
            .cmd = CMD_OFF
        };
        xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Power off command sent to all %d devices", s_device_count);
}

esp_err_t device_manager_get_global_config(global_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(config, &s_global_config, sizeof(global_config_t));
    return ESP_OK;
}

esp_err_t device_manager_set_global_config(const global_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_global_config, config, sizeof(global_config_t));
    nvs_save_global_config(&s_global_config);

    ESP_LOGI(TAG, "Global config updated");
    return ESP_OK;
}

esp_err_t device_manager_add_sensor(uint64_t ieee_addr, uint8_t endpoint,
                                    device_type_t device_type,
                                    const char *manufacturer, const char *model)
{
    if (s_device_count >= MAX_DEVICES) {
        ESP_LOGE(TAG, "Device limit reached (%d)", MAX_DEVICES);
        return ESP_ERR_NO_MEM;
    }

    // Check if device with same IEEE, endpoint AND device_type already exists
    for (uint8_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].ieee_addr == ieee_addr &&
            s_devices[i].endpoint == endpoint &&
            s_devices[i].device_type == device_type) {
            char ieee_str[24];
            format_ieee_addr_str(ieee_str, sizeof(ieee_str), ieee_addr);
            ESP_LOGD(TAG, "Sensor device %s (ep=%d, type=%d) already exists", ieee_str, endpoint, device_type);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    device_config_t *dev = &s_devices[s_device_count];
    memset(dev, 0, sizeof(device_config_t));

    dev->ieee_addr = ieee_addr;
    dev->endpoint = endpoint;
    dev->device_type = device_type;
    dev->enabled = true;  // Sensors always enabled by default

    // Set default sensor configuration from type registry
    const sensor_type_info_t *defaults = sensor_type_get_info(device_type);
    dev->sensor.lower_threshold = defaults ? defaults->default_lower_threshold : 18.0f;
    dev->sensor.upper_threshold = defaults ? defaults->default_upper_threshold : 25.0f;
    dev->sensor.lower_hysteresis = defaults ? defaults->default_hysteresis : 0.5f;
    dev->sensor.upper_hysteresis = defaults ? defaults->default_hysteresis : 0.5f;
    dev->sensor.lower_linked_device = 0;  // Unassigned
    dev->sensor.upper_linked_device = 0;  // Unassigned
    // Event-driven sensors (IAS Zone / leak) use timeout_seconds=0 (disabled)
    dev->sensor.timeout_seconds = (device_type == DEVICE_TYPE_LEAK_SENSOR) ? 0 : 60;

    // Error handling defaults
    dev->sensor.error_linked_device = 0;   // Unassigned
    dev->sensor.error_action_on = false;   // OFF on error

    // Report configuration defaults
    dev->sensor.report_min_interval = 0;   // Min 0 seconds
    dev->sensor.report_max_interval = 10;  // Max 10 seconds
    dev->sensor.report_change = 50;        // 0.50 unit change

    // Delay defaults (0 = immediate, backward compatible)
    dev->sensor.lower_delay_seconds = 0;
    dev->sensor.upper_delay_seconds = 0;

    // Error handling defaults
    dev->sensor.error_linked_device = 0;   // Unassigned
    dev->sensor.error_action_on = false;   // OFF on error

    // Report configuration defaults
    dev->sensor.report_min_interval = 0;   // Min 0 seconds
    dev->sensor.report_max_interval = 10;  // Max 10 seconds
    dev->sensor.report_change = 50;        // 0.50 unit change

    // Delay defaults (0 = immediate, backward compatible)
    dev->sensor.lower_delay_seconds = 0;
    dev->sensor.upper_delay_seconds = 0;

    // Runtime state
    dev->sensor.lower_active = false;
    dev->sensor.upper_active = false;
    dev->sensor.last_lower_action = 0;
    dev->sensor.last_upper_action = 0;
    dev->sensor.lower_delay_pending = false;
    dev->sensor.upper_delay_pending = false;
    dev->sensor.lower_delay_start = 0;
    dev->sensor.upper_delay_start = 0;

    // Reading (runtime only)
    dev->reading.raw_value = 0;
    dev->reading.converted_value = 0.0f;
    dev->reading.last_update = 0;
    dev->reading.valid = false;
    dev->reading.battery_percent = 0xFF;       // Not available
    dev->reading.battery_voltage_100mv = 0xFF; // Not available

    if (manufacturer != NULL) {
        strncpy(dev->manufacturer, manufacturer, MAX_MANUFACTURER_LEN - 1);
    }
    if (model != NULL) {
        strncpy(dev->model, model, MAX_MODEL_LEN - 1);
    }

    const sensor_type_info_t *type_info = sensor_type_get_info(device_type);
    const char *type_str = type_info ? type_info->display_name : "Unknown";
    snprintf(dev->custom_name, MAX_DEVICE_NAME_LEN, "%sSensor%d", type_str, s_device_count + 1);

    s_device_count++;
    s_global_config.device_count = s_device_count;

    // Save to NVS
    nvs_save_device(s_device_count - 1, dev);
    nvs_save_device_count(s_device_count);
    nvs_save_global_config(&s_global_config);

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    char ieee_str[24];
    format_ieee_addr_str(ieee_str, sizeof(ieee_str), ieee_addr);
    ESP_LOGI(TAG, "Added %s sensor %s (ep=%d) at index %d", type_str, ieee_str, endpoint, s_device_count - 1);
    return ESP_OK;
}

esp_err_t device_manager_update_sensor_reading(uint64_t ieee_addr, uint8_t endpoint,
                                               int16_t raw_value, float converted_value,
                                               device_type_t device_type)
{
    bool found = false;
    uint32_t now = (uint32_t)time(NULL);

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    // Update all sensors with matching IEEE + endpoint (multi-cluster support)
    for (uint8_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].ieee_addr == ieee_addr &&
            s_devices[i].endpoint == endpoint &&
            is_sensor_device(s_devices[i].device_type)) {

            // Update last_update for ALL matching sensors
            s_devices[i].reading.last_update = now;
            s_devices[i].reading.valid = true;

            // Update raw/converted values ONLY for the matching device_type
            if (s_devices[i].device_type == device_type) {
                s_devices[i].reading.raw_value = raw_value;
                s_devices[i].reading.converted_value = converted_value;
                found = true;
            }

            // Clear timeout error if it exists
            if (s_errors[i].active && strstr(s_errors[i].error_message, "timeout") != NULL) {
                memset(&s_errors[i], 0, sizeof(device_error_t));
                nvs_save_error(i, &s_errors[i]);
            }
        }
    }

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_find_by_ieee_and_endpoint(uint64_t ieee_addr, uint8_t endpoint,
                                                    device_config_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].ieee_addr == ieee_addr && s_devices[i].endpoint == endpoint) {
            memcpy(device, &s_devices[i], sizeof(device_config_t));
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_get_sensors(device_config_t *sensors, uint8_t *count)
{
    if (sensors == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    for (uint8_t i = 0; i < s_device_count; i++) {
        if (is_sensor_device(s_devices[i].device_type)) {
            memcpy(&sensors[*count], &s_devices[i], sizeof(device_config_t));
            (*count)++;
        }
    }

    return ESP_OK;
}

esp_err_t device_manager_update_battery(uint64_t ieee_addr, uint8_t battery_percent, uint8_t battery_voltage_100mv)
{
    bool found = false;

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    for (uint8_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].ieee_addr == ieee_addr &&
            is_sensor_device(s_devices[i].device_type)) {
            if (battery_percent != 0xFF) {
                s_devices[i].reading.battery_percent = battery_percent;
            }
            if (battery_voltage_100mv != 0xFF) {
                s_devices[i].reading.battery_voltage_100mv = battery_voltage_100mv;
            }
            found = true;
        }
    }

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
