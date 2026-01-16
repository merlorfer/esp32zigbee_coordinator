/**
 * @file main.c
 * @brief ESP32-C6 Zigbee Gateway & Automation Center - Main Entry Point
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"

#include "common.h"
#include "nvs_manager.h"
#include "device_manager.h"
#include "led_task.h"
#include "button_task.h"
#include "wifi_task.h"
#include "zigbee_task.h"
#include "scheduler_task.h"

static const char *TAG = "MAIN";

// ============================================================================
// Global Variables
// ============================================================================

EventGroupHandle_t g_event_group = NULL;
QueueHandle_t g_cmd_queue = NULL;
QueueHandle_t g_led_queue = NULL;
QueueHandle_t g_error_queue = NULL;
SemaphoreHandle_t g_nvs_mutex = NULL;
SemaphoreHandle_t g_device_mutex = NULL;

static bool s_wifi_mode = true;  // Start in Wi-Fi mode

// ============================================================================
// Button Callback
// ============================================================================

static void on_button_press(void)
{
    ESP_LOGI(TAG, "Button pressed - toggling mode");

    if (s_wifi_mode) {
        // Switch to Zigbee mode
        ESP_LOGI(TAG, "Switching to Zigbee automation mode");
        wifi_task_stop();
        scheduler_task_resume();
        s_wifi_mode = false;

        xEventGroupSetBits(g_event_group, EVENT_ZIGBEE_MODE_BIT);
        xEventGroupClearBits(g_event_group, EVENT_WIFI_MODE_BIT);

        // Set LED state based on RTC
        if (wifi_task_is_rtc_initialized()) {
            led_set_state(LED_STATE_NORMAL);
        } else {
            led_set_state(LED_STATE_RTC_NOT_SET);
        }
    } else {
        // Switch to Wi-Fi mode
        ESP_LOGI(TAG, "Switching to Wi-Fi configuration mode");
        scheduler_task_stop();
        wifi_task_start();
        s_wifi_mode = true;

        xEventGroupSetBits(g_event_group, EVENT_WIFI_MODE_BIT);
        xEventGroupClearBits(g_event_group, EVENT_ZIGBEE_MODE_BIT);

        led_set_state(LED_STATE_WIFI_ACTIVE);
    }
}

// ============================================================================
// Initialization
// ============================================================================

static esp_err_t init_globals(void)
{
    // Create event group
    g_event_group = xEventGroupCreate();
    if (g_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }

    // Create queues
    g_cmd_queue = xQueueCreate(16, sizeof(cmd_queue_msg_t));
    if (g_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_FAIL;
    }

    g_led_queue = xQueueCreate(8, sizeof(led_state_t));
    if (g_led_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LED queue");
        return ESP_FAIL;
    }

    g_error_queue = xQueueCreate(8, sizeof(error_queue_msg_t));
    if (g_error_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create error queue");
        return ESP_FAIL;
    }

    // Create mutexes
    g_nvs_mutex = xSemaphoreCreateMutex();
    if (g_nvs_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create NVS mutex");
        return ESP_FAIL;
    }

    g_device_mutex = xSemaphoreCreateMutex();
    if (g_device_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create device mutex");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Global resources initialized");
    return ESP_OK;
}

// ============================================================================
// Main Entry Point
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-C6 Zigbee Gateway & Automation Center");
    ESP_LOGI(TAG, "========================================");

    // Initialize global resources
    ESP_ERROR_CHECK(init_globals());

    // Initialize NVS
    ESP_ERROR_CHECK(nvs_manager_init());

    // Initialize device manager (loads saved devices)
    ESP_ERROR_CHECK(device_manager_init());

    // Initialize LED task
    ESP_ERROR_CHECK(led_task_init());

    // Initialize button task
    ESP_ERROR_CHECK(button_task_init(on_button_press));

    // Initialize Wi-Fi task
    ESP_ERROR_CHECK(wifi_task_init());

    // Initialize Zigbee task
    ESP_ERROR_CHECK(zigbee_task_init());

    // Initialize scheduler task
    ESP_ERROR_CHECK(scheduler_task_init());

    // Start Zigbee (always running per spec)
    ESP_ERROR_CHECK(zigbee_task_start());

    // Start scheduler
    ESP_ERROR_CHECK(scheduler_task_start());
    scheduler_task_stop();  // Initially stopped until Wi-Fi AP is turned off

    // Start in Wi-Fi AP mode (as per user preference)
    ESP_LOGI(TAG, "Starting in Wi-Fi AP mode");
    ESP_ERROR_CHECK(wifi_task_start());
    s_wifi_mode = true;

    xEventGroupSetBits(g_event_group, EVENT_WIFI_MODE_BIT);
    led_set_state(LED_STATE_WIFI_ACTIVE);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "System initialization complete");
    ESP_LOGI(TAG, "Wi-Fi SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "Wi-Fi Password: %s", WIFI_PASSWORD);
    ESP_LOGI(TAG, "Web interface: http://%s", WIFI_AP_IP);
    ESP_LOGI(TAG, "Press button on GPIO %d to toggle mode", GPIO_BUTTON_TOGGLE);
    ESP_LOGI(TAG, "========================================");

    // Main task can now idle - all work is done in other tasks
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        // Log system status periodically
        ESP_LOGD(TAG, "System running - Wi-Fi:%s, RTC:%s, Devices:%d",
                 s_wifi_mode ? "ON" : "OFF",
                 wifi_task_is_rtc_initialized() ? "SET" : "NOT SET",
                 device_manager_get_count());
    }
}
