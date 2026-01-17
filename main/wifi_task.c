/**
 * @file wifi_task.c
 * @brief Wi-Fi SoftAP and HTTP server task implementation
 */

#include "wifi_task.h"
#include "device_manager.h"
#include "led_task.h"
#include "nvs_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "WIFI_TASK";

static httpd_handle_t s_server = NULL;
static bool s_wifi_active = false;
static bool s_rtc_initialized = false;

// External declarations
extern EventGroupHandle_t g_event_group;
extern QueueHandle_t g_cmd_queue;

// Forward declarations
static esp_err_t init_spiffs(void);
static esp_err_t start_webserver(void);
static void stop_webserver(void);

// ============================================================================
// SPIFFS Initialization
// ============================================================================

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/web",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format SPIFFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", total, used);
    }

    return ESP_OK;
}

// ============================================================================
// Wi-Fi Event Handler
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

// ============================================================================
// HTTP Handlers - Static Files
// ============================================================================

static esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type)
{
    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    return serve_file(req, "/web/index.html", "text/html");
}

static esp_err_t style_get_handler(httpd_req_t *req)
{
    return serve_file(req, "/web/style.css", "text/css");
}

static esp_err_t script_get_handler(httpd_req_t *req)
{
    return serve_file(req, "/web/script.js", "application/javascript");
}

// ============================================================================
// HTTP Handlers - API Endpoints
// ============================================================================

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    char time_str[32];
    wifi_task_get_rtc_string(time_str, sizeof(time_str));

    cJSON_AddBoolToObject(root, "rtc_initialized", s_rtc_initialized);
    cJSON_AddStringToObject(root, "current_time", time_str);
    cJSON_AddNumberToObject(root, "device_count", device_manager_get_count());
    cJSON_AddBoolToObject(root, "wifi_active", s_wifi_active);

    EventBits_t bits = xEventGroupGetBits(g_event_group);
    cJSON_AddBoolToObject(root, "zigbee_active", (bits & EVENT_ZIGBEE_MODE_BIT) != 0);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_rtc_set_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *datetime = cJSON_GetObjectItem(root, "datetime");
    if (!cJSON_IsString(datetime)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing datetime");
        return ESP_FAIL;
    }

    int year, month, day, hour, minute, second;
    if (sscanf(datetime->valuestring, "%d-%d-%d %d:%d:%d",
               &year, &month, &day, &hour, &minute, &second) != 6) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid datetime format");
        return ESP_FAIL;
    }

    cJSON_Delete(root);

    esp_err_t err = wifi_task_set_rtc(year, month, day, hour, minute, second);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message",
                           err == ESP_OK ? "RTC beallitva" : "RTC beallitas sikertelen");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t api_zigbee_permit_join_post_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    int duration = ZIGBEE_PERMIT_JOIN_TIME;

    if (root != NULL) {
        cJSON *dur = cJSON_GetObjectItem(root, "duration");
        if (cJSON_IsNumber(dur)) {
            duration = dur->valueint;
        }
        cJSON_Delete(root);
    }

    // TODO: Send permit join command to Zigbee task
    ESP_LOGI(TAG, "Permit join requested for %d seconds", duration);

    char time_str[32];
    time_t now = time(NULL) + duration;
    struct tm *tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "expires_at", time_str);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t api_devices_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *devices = cJSON_CreateArray();

    uint8_t count = device_manager_get_count();
    for (uint8_t i = 0; i < count; i++) {
        device_config_t dev;
        if (device_manager_get_by_index(i, &dev) != ESP_OK) {
            continue;
        }

        cJSON *device = cJSON_CreateObject();

        char addr_str[20];
        snprintf(addr_str, sizeof(addr_str), "0x%016llX", (unsigned long long)dev.ieee_addr);
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
            cJSON_AddNumberToObject(device, "delay_on_minutes", dev.delay_on_minutes);
            cJSON_AddNumberToObject(device, "delay_duration_minutes", dev.delay_duration_minutes);
        }

        cJSON_AddBoolToObject(device, "current_state", dev.current_state);

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

    cJSON_AddItemToObject(root, "devices", devices);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_device_config_post_handler(httpd_req_t *req)
{
    // Extract IEEE address from URI
    char uri[64];
    strncpy(uri, req->uri, sizeof(uri) - 1);
    uri[sizeof(uri) - 1] = '\0';

    // URI format: /api/devices/0x.../config
    char *addr_start = strstr(uri, "/api/devices/") + 13;
    char *addr_end = strstr(addr_start, "/config");
    if (addr_end == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    *addr_end = '\0';

    // Use base 0 for auto-detection of hex prefix (0x)
    uint64_t ieee_addr = strtoull(addr_start, NULL, 0);
    ESP_LOGI(TAG, "Device config request for IEEE addr: 0x%016llX", (unsigned long long)ieee_addr);

    // Read request body
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    device_config_t dev;
    if (device_manager_get(ieee_addr, &dev) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Device not found");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Update fields if present
    cJSON *item;

    item = cJSON_GetObjectItem(root, "custom_name");
    if (cJSON_IsString(item)) {
        strncpy(dev.custom_name, item->valuestring, MAX_DEVICE_NAME_LEN - 1);
    }

    item = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(item)) {
        dev.enabled = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(item)) {
        if (strcmp(item->valuestring, "fixed_time") == 0) {
            dev.mode = MODE_FIXED_TIME;
        } else if (strcmp(item->valuestring, "delay") == 0) {
            dev.mode = MODE_DELAY;
        }
    }

    item = cJSON_GetObjectItem(root, "delay_on_minutes");
    if (cJSON_IsNumber(item)) {
        dev.delay_on_minutes = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "delay_duration_minutes");
    if (cJSON_IsNumber(item)) {
        dev.delay_duration_minutes = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "time_pairs");
    if (cJSON_IsArray(item)) {
        int count = cJSON_GetArraySize(item);
        if (count > MAX_TIME_PAIRS) count = MAX_TIME_PAIRS;
        dev.time_pair_count = count;

        for (int i = 0; i < count; i++) {
            cJSON *pair = cJSON_GetArrayItem(item, i);
            cJSON *on = cJSON_GetObjectItem(pair, "on");
            cJSON *off = cJSON_GetObjectItem(pair, "off");

            if (cJSON_IsString(on)) {
                sscanf(on->valuestring, "%hhu:%hhu",
                       &dev.time_pairs[i].on_time.hour,
                       &dev.time_pairs[i].on_time.minute);
            }
            if (cJSON_IsString(off)) {
                sscanf(off->valuestring, "%hhu:%hhu",
                       &dev.time_pairs[i].off_time.hour,
                       &dev.time_pairs[i].off_time.minute);
            }
        }
    }

    cJSON_Delete(root);

    device_manager_update(ieee_addr, &dev);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Eszkoz konfiguracio frissitve");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t api_device_delete_handler(httpd_req_t *req)
{
    // Extract IEEE address from URI
    char uri[64];
    strncpy(uri, req->uri, sizeof(uri) - 1);
    uri[sizeof(uri) - 1] = '\0';

    // URI format: /api/devices/0x...
    char *addr_start = strstr(uri, "/api/devices/") + 13;

    // Use base 0 for auto-detection of hex prefix (0x)
    uint64_t ieee_addr = strtoull(addr_start, NULL, 0);
    ESP_LOGI(TAG, "Delete device request for IEEE addr: 0x%016llX", (unsigned long long)ieee_addr);

    esp_err_t err = device_manager_remove(ieee_addr);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddStringToObject(response, "message",
                           err == ESP_OK ? "Eszkoz torolve" : "Eszkoz nem talalhato");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t api_wifi_shutdown_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (root != NULL) {
            cJSON *behavior = cJSON_GetObjectItem(root, "wifi_on_behavior");
            if (cJSON_IsString(behavior)) {
                global_config_t config;
                device_manager_get_global_config(&config);
                config.wifi_on_behavior = (strcmp(behavior->valuestring, "maintain_state") == 0);
                device_manager_set_global_config(&config);
            }
            cJSON_Delete(root);
        }
    }

    // Clear all errors when starting automation
    device_manager_clear_all_errors();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message",
                           "Wi-Fi leallitasa megkezdve, Zigbee automatizacio aktiv");
    cJSON_AddBoolToObject(response, "errors_cleared", true);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    // Schedule Wi-Fi stop after response is sent
    xEventGroupSetBits(g_event_group, EVENT_ZIGBEE_MODE_BIT);
    xEventGroupClearBits(g_event_group, EVENT_WIFI_MODE_BIT);

    return ESP_OK;
}

static esp_err_t api_global_config_get_handler(httpd_req_t *req)
{
    global_config_t config;
    device_manager_get_global_config(&config);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "wifi_on_behavior", config.wifi_on_behavior);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t api_global_config_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    global_config_t config;
    device_manager_get_global_config(&config);

    cJSON *item = cJSON_GetObjectItem(root, "wifi_on_behavior");
    if (cJSON_IsBool(item)) {
        config.wifi_on_behavior = cJSON_IsTrue(item);
    }

    cJSON_Delete(root);

    device_manager_set_global_config(&config);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Globalis konfig mentve");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested!");

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

    // Reset RTC flag
    s_rtc_initialized = false;

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Gyari alaphelyzetbe allitas sikeres");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    ESP_LOGI(TAG, "Factory reset completed");
    return ESP_OK;
}

// ============================================================================
// HTTP Server Setup
// ============================================================================

static esp_err_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Static file handlers
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t style_uri = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = style_get_handler
    };
    httpd_register_uri_handler(s_server, &style_uri);

    httpd_uri_t script_uri = {
        .uri = "/script.js",
        .method = HTTP_GET,
        .handler = script_get_handler
    };
    httpd_register_uri_handler(s_server, &script_uri);

    // API handlers
    httpd_uri_t api_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_get_handler
    };
    httpd_register_uri_handler(s_server, &api_status);

    httpd_uri_t api_rtc_set = {
        .uri = "/api/rtc/set",
        .method = HTTP_POST,
        .handler = api_rtc_set_post_handler
    };
    httpd_register_uri_handler(s_server, &api_rtc_set);

    httpd_uri_t api_permit_join = {
        .uri = "/api/zigbee/permit-join",
        .method = HTTP_POST,
        .handler = api_zigbee_permit_join_post_handler
    };
    httpd_register_uri_handler(s_server, &api_permit_join);

    httpd_uri_t api_devices = {
        .uri = "/api/devices",
        .method = HTTP_GET,
        .handler = api_devices_get_handler
    };
    httpd_register_uri_handler(s_server, &api_devices);

    httpd_uri_t api_device_config = {
        .uri = "/api/devices/*/config",
        .method = HTTP_POST,
        .handler = api_device_config_post_handler
    };
    httpd_register_uri_handler(s_server, &api_device_config);

    httpd_uri_t api_device_delete = {
        .uri = "/api/devices/*",
        .method = HTTP_DELETE,
        .handler = api_device_delete_handler
    };
    httpd_register_uri_handler(s_server, &api_device_delete);

    httpd_uri_t api_wifi_shutdown = {
        .uri = "/api/wifi/shutdown",
        .method = HTTP_POST,
        .handler = api_wifi_shutdown_post_handler
    };
    httpd_register_uri_handler(s_server, &api_wifi_shutdown);

    httpd_uri_t api_global_config_get = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = api_global_config_get_handler
    };
    httpd_register_uri_handler(s_server, &api_global_config_get);

    httpd_uri_t api_global_config_post = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = api_global_config_post_handler
    };
    httpd_register_uri_handler(s_server, &api_global_config_post);

    httpd_uri_t api_factory_reset = {
        .uri = "/api/factory-reset",
        .method = HTTP_POST,
        .handler = api_factory_reset_handler
    };
    httpd_register_uri_handler(s_server, &api_factory_reset);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

static void stop_webserver(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t wifi_task_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    // Initialize SPIFFS
    init_spiffs();

    ESP_LOGI(TAG, "Wi-Fi task initialized");
    return ESP_OK;
}

esp_err_t wifi_task_start(void)
{
    esp_err_t ret;

    // Simple Wi-Fi AP configuration - working on ESP32-C6
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASSWORD,
            .max_connection = WIFI_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,  // Mixed mode for better compatibility
            .ssid_hidden = 0,
            .beacon_interval = 100,
            .pairwise_cipher = WIFI_CIPHER_TYPE_CCMP,
        },
    };

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi config: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set maximum TX power AFTER wifi_start (20 dBm = 80 in quarter dBm units)
    ret = esp_wifi_set_max_tx_power(80);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set TX power: %s", esp_err_to_name(ret));
    }

    // Log actual TX power
    int8_t tx_power;
    esp_wifi_get_max_tx_power(&tx_power);
    ESP_LOGI(TAG, "Wi-Fi TX power: %d (quarter dBm)", tx_power);

    ESP_LOGI(TAG, "Wi-Fi AP started. SSID:%s password:%s", WIFI_SSID, WIFI_PASSWORD);

    // Handle devices based on wifi_on_behavior setting
    global_config_t config;
    device_manager_get_global_config(&config);
    if (!config.wifi_on_behavior) {
        // Power off all devices
        device_manager_power_off_all();
    }

    start_webserver();
    s_wifi_active = true;

    xEventGroupSetBits(g_event_group, EVENT_WIFI_MODE_BIT);
    led_set_state(LED_STATE_WIFI_ACTIVE);

    return ESP_OK;
}

esp_err_t wifi_task_stop(void)
{
    stop_webserver();

    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop Wi-Fi: %s", esp_err_to_name(ret));
    }

    s_wifi_active = false;
    xEventGroupClearBits(g_event_group, EVENT_WIFI_MODE_BIT);
    led_set_state(LED_STATE_NORMAL);

    ESP_LOGI(TAG, "Wi-Fi AP stopped");
    return ESP_OK;
}

bool wifi_task_is_active(void)
{
    return s_wifi_active;
}

esp_err_t wifi_task_set_rtc(int year, int month, int day, int hour, int minute, int second)
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

esp_err_t wifi_task_get_rtc_string(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size < 20) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);

    return ESP_OK;
}

bool wifi_task_is_rtc_initialized(void)
{
    return s_rtc_initialized;
}
