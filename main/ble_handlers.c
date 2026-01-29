/**
 * @file ble_handlers.c
 * @brief BLE command handlers implementation
 *
 * Mirrors HTTP handlers from wifi_task.c but uses JSON-over-BLE protocol.
 */

#include "ble_handlers.h"
#include "ble_task.h"
#include "device_manager.h"
#include "zigbee_task.h"
#include "led_task.h"
#include "common.h"

#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <time.h>

static const char *TAG = "BLE_HANDLERS";

// External declarations
extern EventGroupHandle_t g_event_group;
extern QueueHandle_t g_cmd_queue;

/* Helper function to format IEEE address as hex string */
static void format_ieee_addr_hex(char *buf, size_t buf_size, uint64_t ieee_addr)
{
    snprintf(buf, buf_size, "0x%02X%02X%02X%02X%02X%02X%02X%02X",
             (uint8_t)((ieee_addr >> 56) & 0xFF),
             (uint8_t)((ieee_addr >> 48) & 0xFF),
             (uint8_t)((ieee_addr >> 40) & 0xFF),
             (uint8_t)((ieee_addr >> 32) & 0xFF),
             (uint8_t)((ieee_addr >> 24) & 0xFF),
             (uint8_t)((ieee_addr >> 16) & 0xFF),
             (uint8_t)((ieee_addr >> 8) & 0xFF),
             (uint8_t)(ieee_addr & 0xFF));
}

/* ============================================================================
 * Command Handlers
 * ============================================================================ */

static char *handle_get_status(cJSON *params)
{
    cJSON *root = cJSON_CreateObject();

    char time_str[32];
    ble_task_get_rtc_string(time_str, sizeof(time_str));

    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddBoolToObject(root, "rtc_initialized", ble_task_is_rtc_initialized());
    cJSON_AddStringToObject(root, "current_time", time_str);
    cJSON_AddNumberToObject(root, "device_count", device_manager_get_count());
    cJSON_AddBoolToObject(root, "ble_active", ble_task_is_active());

    EventBits_t bits = xEventGroupGetBits(g_event_group);
    cJSON_AddBoolToObject(root, "zigbee_active", (bits & EVENT_ZIGBEE_MODE_BIT) != 0);
    cJSON_AddBoolToObject(root, "wifi_active", (bits & EVENT_WIFI_MODE_BIT) != 0);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static char *handle_get_devices(cJSON *params)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *devices = cJSON_CreateArray();

    uint8_t count = device_manager_get_count();
    ESP_LOGI(TAG, "get_devices: device count=%d", count);

    for (uint8_t i = 0; i < count; i++) {
        device_config_t dev;
        if (device_manager_get_by_index(i, &dev) != ESP_OK) {
            continue;
        }

        cJSON *device = cJSON_CreateObject();

        char addr_str[20];
        format_ieee_addr_hex(addr_str, sizeof(addr_str), dev.ieee_addr);
        cJSON_AddStringToObject(device, "ieee_addr", addr_str);
        cJSON_AddNumberToObject(device, "endpoint", dev.endpoint);
        cJSON_AddStringToObject(device, "manufacturer", dev.manufacturer);
        cJSON_AddStringToObject(device, "model", dev.model);
        cJSON_AddStringToObject(device, "custom_name", dev.custom_name);
        cJSON_AddBoolToObject(device, "enabled", dev.enabled);
        cJSON_AddStringToObject(device, "mode",
                               dev.mode == MODE_FIXED_TIME ? "fixed_time" : "delay");

        if (dev.mode == MODE_FIXED_TIME) {
            cJSON *time_pairs = cJSON_CreateArray();
            for (int j = 0; j < dev.time_pair_count; j++) {
                cJSON *pair = cJSON_CreateObject();
                char on_time[8], off_time[8];
                snprintf(on_time, sizeof(on_time), "%02d:%02d",
                        dev.time_pairs[j].on_time.hour, dev.time_pairs[j].on_time.minute);
                snprintf(off_time, sizeof(off_time), "%02d:%02d",
                        dev.time_pairs[j].off_time.hour, dev.time_pairs[j].off_time.minute);
                cJSON_AddStringToObject(pair, "on", on_time);
                cJSON_AddStringToObject(pair, "off", off_time);
                cJSON_AddItemToArray(time_pairs, pair);
            }
            cJSON_AddItemToObject(device, "time_pairs", time_pairs);
        } else {
            // 3-phase delay: OFF1 → ON → OFF2
            cJSON_AddNumberToObject(device, "delay_off1_minutes", dev.delay_off1_minutes);
            cJSON_AddNumberToObject(device, "delay_duration_minutes", dev.delay_duration_minutes);
            cJSON_AddNumberToObject(device, "delay_off2_minutes", dev.delay_off2_minutes);
        }

        // Check for error
        device_error_t error;
        if (device_manager_get_error(dev.ieee_addr, &error) == ESP_OK && error.active) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddStringToObject(err, "message", error.error_message);

            struct tm *tm_info = localtime((time_t *)&error.timestamp);
            char ts[8];
            strftime(ts, sizeof(ts), "%H:%M", tm_info);
            cJSON_AddStringToObject(err, "timestamp", ts);

            cJSON_AddItemToObject(device, "error", err);
        } else {
            cJSON_AddNullToObject(device, "error");
        }

        cJSON_AddItemToArray(devices, device);
    }

    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "devices", devices);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static char *handle_set_rtc(cJSON *params)
{
    if (params == NULL) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Missing params");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    cJSON *year_obj = cJSON_GetObjectItem(params, "year");
    cJSON *month_obj = cJSON_GetObjectItem(params, "month");
    cJSON *day_obj = cJSON_GetObjectItem(params, "day");
    cJSON *hour_obj = cJSON_GetObjectItem(params, "hour");
    cJSON *minute_obj = cJSON_GetObjectItem(params, "minute");
    cJSON *second_obj = cJSON_GetObjectItem(params, "second");

    if (!cJSON_IsNumber(year_obj) || !cJSON_IsNumber(month_obj) ||
        !cJSON_IsNumber(day_obj) || !cJSON_IsNumber(hour_obj) ||
        !cJSON_IsNumber(minute_obj) || !cJSON_IsNumber(second_obj)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Invalid time parameters");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    esp_err_t err = ble_task_set_rtc(
        year_obj->valueint,
        month_obj->valueint,
        day_obj->valueint,
        hour_obj->valueint,
        minute_obj->valueint,
        second_obj->valueint
    );

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", err == ESP_OK ? "ok" : "error");
    cJSON_AddStringToObject(response, "message",
                           err == ESP_OK ? "RTC time set" : "RTC set failed");

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json_str;
}

static char *handle_set_device_config(cJSON *params)
{
    if (params == NULL) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Missing params");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    cJSON *ieee_addr_obj = cJSON_GetObjectItem(params, "ieee_addr");
    if (!cJSON_IsString(ieee_addr_obj)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Missing ieee_addr");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    uint64_t ieee_addr = strtoull(ieee_addr_obj->valuestring, NULL, 0);

    device_config_t dev;
    if (device_manager_get(ieee_addr, &dev) != ESP_OK) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Device not found");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    // Update fields if present
    cJSON *item;

    item = cJSON_GetObjectItem(params, "custom_name");
    if (cJSON_IsString(item)) {
        strncpy(dev.custom_name, item->valuestring, MAX_DEVICE_NAME_LEN - 1);
        dev.custom_name[MAX_DEVICE_NAME_LEN - 1] = '\0';
    }

    item = cJSON_GetObjectItem(params, "enabled");
    if (cJSON_IsBool(item)) {
        dev.enabled = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItem(params, "mode");
    if (cJSON_IsString(item)) {
        if (strcmp(item->valuestring, "fixed_time") == 0) {
            dev.mode = MODE_FIXED_TIME;
        } else if (strcmp(item->valuestring, "delay") == 0) {
            dev.mode = MODE_DELAY;
        }
    }

    // 3-phase delay: OFF1 → ON → OFF2
    item = cJSON_GetObjectItem(params, "delay_off1_minutes");
    if (cJSON_IsNumber(item)) {
        dev.delay_off1_minutes = item->valueint;
    }

    item = cJSON_GetObjectItem(params, "delay_duration_minutes");
    if (cJSON_IsNumber(item)) {
        dev.delay_duration_minutes = item->valueint;
    }

    item = cJSON_GetObjectItem(params, "delay_off2_minutes");
    if (cJSON_IsNumber(item)) {
        dev.delay_off2_minutes = item->valueint;
    }

    item = cJSON_GetObjectItem(params, "time_pairs");
    if (cJSON_IsArray(item)) {
        int count = cJSON_GetArraySize(item);
        if (count > MAX_TIME_PAIRS) count = MAX_TIME_PAIRS;
        dev.time_pair_count = count;

        for (int i = 0; i < count; i++) {
            cJSON *pair = cJSON_GetArrayItem(item, i);
            cJSON *on = cJSON_GetObjectItem(pair, "on");
            cJSON *off = cJSON_GetObjectItem(pair, "off");

            if (cJSON_IsString(on)) {
                int on_hour = 0, on_min = 0;
                if (sscanf(on->valuestring, "%d:%d", &on_hour, &on_min) == 2) {
                    dev.time_pairs[i].on_time.hour = (uint8_t)on_hour;
                    dev.time_pairs[i].on_time.minute = (uint8_t)on_min;
                }
            }
            if (cJSON_IsString(off)) {
                int off_hour = 0, off_min = 0;
                if (sscanf(off->valuestring, "%d:%d", &off_hour, &off_min) == 2) {
                    dev.time_pairs[i].off_time.hour = (uint8_t)off_hour;
                    dev.time_pairs[i].off_time.minute = (uint8_t)off_min;
                }
            }
        }
    }

    device_manager_update(ieee_addr, &dev);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddStringToObject(response, "message", "Device config updated");

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json_str;
}

static char *handle_delete_device(cJSON *params)
{
    if (params == NULL) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Missing params");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    cJSON *ieee_addr_obj = cJSON_GetObjectItem(params, "ieee_addr");
    if (!cJSON_IsString(ieee_addr_obj)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Missing ieee_addr");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    uint64_t ieee_addr = strtoull(ieee_addr_obj->valuestring, NULL, 0);

    esp_err_t err = device_manager_remove(ieee_addr);

    if (err == ESP_OK) {
        // Try to send leave request
        zigbee_request_leave(ieee_addr);
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", err == ESP_OK ? "ok" : "error");
    cJSON_AddStringToObject(response, "message",
                           err == ESP_OK ? "Device deleted" : "Device not found");

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json_str;
}

static char *handle_control_device(cJSON *params)
{
    if (params == NULL) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Missing params");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    cJSON *ieee_addr_obj = cJSON_GetObjectItem(params, "ieee_addr");
    cJSON *endpoint_obj = cJSON_GetObjectItem(params, "endpoint");
    cJSON *cmd_obj = cJSON_GetObjectItem(params, "cmd");

    if (!cJSON_IsString(ieee_addr_obj) || !cJSON_IsNumber(endpoint_obj) ||
        !cJSON_IsString(cmd_obj)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Invalid parameters");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    uint64_t ieee_addr = strtoull(ieee_addr_obj->valuestring, NULL, 0);
    uint8_t endpoint = (uint8_t)endpoint_obj->valueint;

    zigbee_cmd_type_t cmd_type;
    if (strcmp(cmd_obj->valuestring, "on") == 0) {
        cmd_type = CMD_ON;
    } else if (strcmp(cmd_obj->valuestring, "off") == 0) {
        cmd_type = CMD_OFF;
    } else if (strcmp(cmd_obj->valuestring, "toggle") == 0) {
        cmd_type = CMD_TOGGLE;
    } else {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Invalid command");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    // Send command to Zigbee task
    cmd_queue_msg_t msg = {
        .ieee_addr = ieee_addr,
        .endpoint = endpoint,
        .cmd = cmd_type
    };

    if (xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Command queue full");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddStringToObject(response, "message", "Command sent");

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json_str;
}

static char *handle_permit_join(cJSON *params)
{
    int duration = ZIGBEE_PERMIT_JOIN_TIME;

    if (params != NULL) {
        cJSON *dur = cJSON_GetObjectItem(params, "duration");
        if (cJSON_IsNumber(dur)) {
            duration = dur->valueint;
        }
    }

    esp_err_t err = zigbee_permit_join(duration);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", err == ESP_OK ? "ok" : "error");
    cJSON_AddStringToObject(response, "message",
                           err == ESP_OK ? "Pairing enabled" : "Failed to enable pairing");

    if (err == ESP_OK) {
        cJSON_AddNumberToObject(response, "duration", duration);
        led_set_state(LED_STATE_PAIRING);
    }

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json_str;
}

static char *handle_get_global_settings(cJSON *params)
{
    global_config_t config;
    device_manager_get_global_config(&config);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddBoolToObject(response, "wifi_on_behavior", config.wifi_on_behavior);

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json_str;
}

static char *handle_set_global_settings(cJSON *params)
{
    if (params == NULL) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Missing params");
        char *json_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return json_str;
    }

    global_config_t config;
    device_manager_get_global_config(&config);

    cJSON *item = cJSON_GetObjectItem(params, "wifi_on_behavior");
    if (cJSON_IsBool(item)) {
        config.wifi_on_behavior = cJSON_IsTrue(item);
    }

    device_manager_set_global_config(&config);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddStringToObject(response, "message", "Global settings updated");

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json_str;
}

static char *handle_factory_reset(cJSON *params)
{
    ESP_LOGW(TAG, "Factory reset requested via BLE");

    // Clear all devices
    uint8_t count = device_manager_get_count();
    for (int i = count - 1; i >= 0; i--) {
        device_config_t dev;
        if (device_manager_get_by_index(i, &dev) == ESP_OK) {
            device_manager_remove(dev.ieee_addr);
        }
    }

    // Clear all errors
    device_manager_clear_all_errors();

    // Reset global config
    global_config_t config = {0};
    device_manager_set_global_config(&config);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddStringToObject(response, "message", "Factory reset successful");

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    ESP_LOGI(TAG, "Factory reset completed via BLE");
    return json_str;
}

/* ============================================================================
 * Main Command Router
 * ============================================================================ */

char *ble_handlers_process_command(const char *json_str, size_t len)
{
    if (json_str == NULL || len == 0) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Empty command");
        char *error_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return error_str;
    }

    ESP_LOGI(TAG, "Processing command: %.*s", (int)len, json_str);

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Invalid JSON");
        char *error_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return error_str;
    }

    cJSON *cmd_obj = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd_obj)) {
        cJSON_Delete(root);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Missing cmd field");
        char *error_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return error_str;
    }

    const char *cmd = cmd_obj->valuestring;
    cJSON *params = cJSON_GetObjectItem(root, "params");

    char *response = NULL;

    if (strcmp(cmd, "get_status") == 0) {
        response = handle_get_status(params);
    } else if (strcmp(cmd, "get_devices") == 0) {
        response = handle_get_devices(params);
    } else if (strcmp(cmd, "set_rtc") == 0) {
        response = handle_set_rtc(params);
    } else if (strcmp(cmd, "set_device_config") == 0) {
        response = handle_set_device_config(params);
    } else if (strcmp(cmd, "delete_device") == 0) {
        response = handle_delete_device(params);
    } else if (strcmp(cmd, "control_device") == 0) {
        response = handle_control_device(params);
    } else if (strcmp(cmd, "permit_join") == 0) {
        response = handle_permit_join(params);
    } else if (strcmp(cmd, "get_global_settings") == 0) {
        response = handle_get_global_settings(params);
    } else if (strcmp(cmd, "set_global_settings") == 0) {
        response = handle_set_global_settings(params);
    } else if (strcmp(cmd, "factory_reset") == 0) {
        response = handle_factory_reset(params);
    } else {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "status", "error");
        cJSON_AddStringToObject(error, "message", "Unknown command");
        response = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
    }

    cJSON_Delete(root);
    return response;
}
