/**
 * @file nvs_manager.c
 * @brief NVS operations implementation
 */

#include "nvs_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "NVS_MANAGER";

esp_err_t nvs_manager_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized successfully");
    } else {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t nvs_save_global_config(const global_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, NVS_KEY_GLOBAL, config, sizeof(global_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save global config: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Global config saved successfully");
    }

    return ret;
}

esp_err_t nvs_load_global_config(global_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_CONFIG, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace for reading: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t size = sizeof(global_config_t);
    ret = nvs_get_blob(handle, NVS_KEY_GLOBAL, config, &size);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Global config loaded successfully");
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No saved global config found, using defaults");
        memset(config, 0, sizeof(global_config_t));
        config->wifi_on_behavior = false;  // Default: power off devices on WiFi AP start
        config->device_count = 0;
        config->rtc_initialized = false;
    }

    return ret;
}

esp_err_t nvs_save_device(uint8_t index, const device_config_t *device)
{
    if (device == NULL || index >= MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_DEVICES, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open devices namespace: %s", esp_err_to_name(ret));
        return ret;
    }

    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_DEVICE_PREFIX, index);

    ret = nvs_set_blob(handle, key, device, sizeof(device_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device %d: %s", index, esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Device %d saved successfully", index);
    }

    return ret;
}

esp_err_t nvs_load_device(uint8_t index, device_config_t *device)
{
    if (device == NULL || index >= MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_DEVICES, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_DEVICE_PREFIX, index);

    size_t size = sizeof(device_config_t);
    ret = nvs_get_blob(handle, key, device, &size);
    nvs_close(handle);

    return ret;
}

esp_err_t nvs_delete_device(uint8_t index)
{
    if (index >= MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_DEVICES, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_DEVICE_PREFIX, index);

    ret = nvs_erase_key(handle, key);
    if (ret == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "Device %d deleted", index);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_save_error(uint8_t index, const device_error_t *error)
{
    if (error == NULL || index >= MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_ERRORS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_ERROR_PREFIX, index);

    ret = nvs_set_blob(handle, key, error, sizeof(device_error_t));
    if (ret == ESP_OK) {
        nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_load_error(uint8_t index, device_error_t *error)
{
    if (error == NULL || index >= MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_ERRORS, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        memset(error, 0, sizeof(device_error_t));
        return ret;
    }

    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_ERROR_PREFIX, index);

    size_t size = sizeof(device_error_t);
    ret = nvs_get_blob(handle, key, error, &size);
    nvs_close(handle);

    if (ret != ESP_OK) {
        memset(error, 0, sizeof(device_error_t));
    }

    return ret;
}

esp_err_t nvs_clear_all_errors(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_ERRORS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "All errors cleared");
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_save_device_count(uint8_t count)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_DEVICES, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(handle, NVS_KEY_DEVICE_COUNT, count);
    if (ret == ESP_OK) {
        nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_load_device_count(uint8_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_DEVICES, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        *count = 0;
        return ret;
    }

    ret = nvs_get_u8(handle, NVS_KEY_DEVICE_COUNT, count);
    nvs_close(handle);

    if (ret != ESP_OK) {
        *count = 0;
    }

    return ret;
}
