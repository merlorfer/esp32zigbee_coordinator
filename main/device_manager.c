/**
 * @file device_manager.c
 * @brief Device list management implementation
 */

#include "device_manager.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "DEVICE_MANAGER";

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

    // Check if device already exists
    if (device_manager_exists(ieee_addr)) {
        ESP_LOGW(TAG, "Device 0x%016llX already exists", (unsigned long long)ieee_addr);
        return ESP_ERR_INVALID_STATE;
    }

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    device_config_t *dev = &s_devices[s_device_count];
    memset(dev, 0, sizeof(device_config_t));

    dev->ieee_addr = ieee_addr;
    dev->endpoint = endpoint;
    dev->enabled = false;  // Default: automation disabled
    dev->mode = MODE_FIXED_TIME;
    dev->time_pair_count = 1;
    dev->time_pairs[0].on_time.hour = 6;
    dev->time_pairs[0].on_time.minute = 0;
    dev->time_pairs[0].off_time.hour = 18;
    dev->time_pairs[0].off_time.minute = 0;
    dev->delay_on_minutes = 30;
    dev->delay_duration_minutes = 120;
    dev->current_state = false;

    if (manufacturer != NULL) {
        strncpy(dev->manufacturer, manufacturer, MAX_MANUFACTURER_LEN - 1);
    }
    if (model != NULL) {
        strncpy(dev->model, model, MAX_MODEL_LEN - 1);
    }

    snprintf(dev->custom_name, MAX_DEVICE_NAME_LEN, "Device %d", s_device_count + 1);

    s_device_count++;
    s_global_config.device_count = s_device_count;

    // Save to NVS
    nvs_save_device(s_device_count - 1, dev);
    nvs_save_device_count(s_device_count);
    nvs_save_global_config(&s_global_config);

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    ESP_LOGI(TAG, "Added device 0x%016llX at index %d",
             (unsigned long long)ieee_addr, s_device_count - 1);
    return ESP_OK;
}

esp_err_t device_manager_remove(uint64_t ieee_addr)
{
    int index = device_manager_find_index(ieee_addr);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (g_device_mutex != NULL) {
        xSemaphoreTake(g_device_mutex, portMAX_DELAY);
    }

    // Shift remaining devices down
    for (int i = index; i < s_device_count - 1; i++) {
        memcpy(&s_devices[i], &s_devices[i + 1], sizeof(device_config_t));
        memcpy(&s_errors[i], &s_errors[i + 1], sizeof(device_error_t));
    }

    // Clear last slot
    memset(&s_devices[s_device_count - 1], 0, sizeof(device_config_t));
    memset(&s_errors[s_device_count - 1], 0, sizeof(device_error_t));

    s_device_count--;
    s_global_config.device_count = s_device_count;

    // Save all devices to NVS (indices changed)
    for (uint8_t i = 0; i < s_device_count; i++) {
        nvs_save_device(i, &s_devices[i]);
        nvs_save_error(i, &s_errors[i]);
    }
    nvs_delete_device(s_device_count);
    nvs_save_device_count(s_device_count);
    nvs_save_global_config(&s_global_config);

    if (g_device_mutex != NULL) {
        xSemaphoreGive(g_device_mutex);
    }

    ESP_LOGI(TAG, "Removed device at index %d, %d devices remaining",
             index, s_device_count);
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

    ESP_LOGI(TAG, "Updated device 0x%016llX", (unsigned long long)ieee_addr);
    return ESP_OK;
}

esp_err_t device_manager_set_state(uint64_t ieee_addr, bool state)
{
    int index = device_manager_find_index(ieee_addr);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    s_devices[index].current_state = state;
    s_devices[index].last_command_timestamp = (uint32_t)time(NULL);

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

    ESP_LOGW(TAG, "Error set for device 0x%016llX: %s",
             (unsigned long long)ieee_addr, error_message ? error_message : "unknown");
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
