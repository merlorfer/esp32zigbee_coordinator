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

    // Create a copy of the device to clear runtime-only fields
    device_config_t device_copy;
    memcpy(&device_copy, device, sizeof(device_config_t));

    // Clear runtime-only sensor reading data (should not be persisted)
    device_copy.reading.raw_value = 0;
    device_copy.reading.converted_value = 0.0f;
    device_copy.reading.last_update = 0;
    device_copy.reading.valid = false;

    // Also clear runtime sensor state (not persisted)
    device_copy.sensor.lower_active = false;
    device_copy.sensor.upper_active = false;
    device_copy.sensor.last_lower_action = 0;
    device_copy.sensor.last_upper_action = 0;
    device_copy.sensor.lower_delay_pending = false;
    device_copy.sensor.upper_delay_pending = false;
    device_copy.sensor.lower_delay_start = 0;
    device_copy.sensor.upper_delay_start = 0;

    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_DEVICE_PREFIX, index);

    ret = nvs_set_blob(handle, key, &device_copy, sizeof(device_config_t));
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

    if (ret == ESP_OK) {
        // Initialize runtime-only fields after loading
        device->reading.raw_value = 0;
        device->reading.converted_value = 0.0f;
        device->reading.last_update = 0;
        device->reading.valid = false;

        // Initialize runtime sensor state
        device->sensor.lower_active = false;
        device->sensor.upper_active = false;
        device->sensor.last_lower_action = 0;
        device->sensor.last_upper_action = 0;
        device->sensor.lower_delay_pending = false;
        device->sensor.upper_delay_pending = false;
        device->sensor.lower_delay_start = 0;
        device->sensor.upper_delay_start = 0;

        // Handle migration for devices saved before sensor support was added
        // If device_type was not set (old version), default to ON_OFF_LIGHT
        // This is a simple migration - newer code will have device_type set properly
    }

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

/* ============================================================================
 * Rules Engine NVS Functions
 * ============================================================================ */

#define NVS_NAMESPACE_RULES     "rules"
#define NVS_KEY_RULES_TEXT      "rules_txt"
#define NVS_KEY_RULES_VARS      "rules_vars"
#define NVS_KEY_RULES_VCFG      "rules_vcfg"

typedef struct {
    uint8_t persist_mask;
    float   defaults[8];
} rules_var_config_nvs_t;

esp_err_t nvs_save_rules_text(const char *text, size_t len)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_RULES, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open rules namespace: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, NVS_KEY_RULES_TEXT, text, len + 1);  // +1 for null terminator
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Rules text saved (%d bytes)", (int)len);
        }
    } else {
        ESP_LOGE(TAG, "Failed to save rules text: %s", esp_err_to_name(ret));
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_load_rules_text(char *text, size_t *len)
{
    if (text == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_RULES, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        text[0] = '\0';
        *len = 0;
        return ret;
    }

    ret = nvs_get_blob(handle, NVS_KEY_RULES_TEXT, text, len);
    nvs_close(handle);

    if (ret != ESP_OK) {
        text[0] = '\0';
        *len = 0;
    }

    return ret;
}

esp_err_t nvs_save_rules_vars(const float *vars, uint8_t count)
{
    if (vars == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_RULES, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(handle, NVS_KEY_RULES_VARS, vars, count * sizeof(float));
    if (ret == ESP_OK) {
        nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_load_rules_vars(float *vars, uint8_t count)
{
    if (vars == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(vars, 0, count * sizeof(float));

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_RULES, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t size = count * sizeof(float);
    ret = nvs_get_blob(handle, NVS_KEY_RULES_VARS, vars, &size);
    nvs_close(handle);

    return ret;
}

esp_err_t nvs_save_rules_var_config(uint8_t persist_mask, const float *defaults, uint8_t count)
{
    if (defaults == NULL || count > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    rules_var_config_nvs_t cfg;
    cfg.persist_mask = persist_mask;
    memset(cfg.defaults, 0, sizeof(cfg.defaults));
    memcpy(cfg.defaults, defaults, count * sizeof(float));

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_RULES, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(handle, NVS_KEY_RULES_VCFG, &cfg, sizeof(cfg));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Rules var config saved (mask=0x%02X)", persist_mask);
        }
    } else {
        ESP_LOGE(TAG, "Failed to save rules var config: %s", esp_err_to_name(ret));
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_load_rules_var_config(uint8_t *persist_mask, float *defaults, uint8_t count)
{
    if (persist_mask == NULL || defaults == NULL || count > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    // Defaults: all persist, all default=0
    *persist_mask = 0xFF;
    memset(defaults, 0, count * sizeof(float));

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_RULES, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    rules_var_config_nvs_t cfg;
    size_t size = sizeof(cfg);
    ret = nvs_get_blob(handle, NVS_KEY_RULES_VCFG, &cfg, &size);
    nvs_close(handle);

    if (ret == ESP_OK) {
        *persist_mask = cfg.persist_mask;
        memcpy(defaults, cfg.defaults, count * sizeof(float));
        ESP_LOGI(TAG, "Rules var config loaded (mask=0x%02X)", cfg.persist_mask);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Not stored yet - use defaults (backward compatible)
        ret = ESP_ERR_NVS_NOT_FOUND;
    }

    return ret;
}
