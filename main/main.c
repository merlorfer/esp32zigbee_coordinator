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
#include "freertos/timers.h"
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
static bool s_pairing_mode = false;  // Track if in pairing mode
static TimerHandle_t s_pairing_timer = NULL;  // Timer for pairing timeout

// ============================================================================
// Pairing Mode Timer Callback
// ============================================================================

static void pairing_timer_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Pairing mode timeout - disabling permit join");
    s_pairing_mode = false;

    // Restore LED state based on current mode
    if (s_wifi_mode) {
        led_set_state(LED_STATE_WIFI_ACTIVE);
    } else {
        led_set_state(LED_STATE_NORMAL);
    }
}

// ============================================================================
// Button Callbacks
// ============================================================================

static bool s_zigbee_started = false;  // Track if Zigbee task was ever started

static void on_button_short_press(void)
{
    ESP_LOGI(TAG, "Short press - toggling mode");

    // If in pairing mode, cancel it first
    if (s_pairing_mode) {
        ESP_LOGI(TAG, "Cancelling pairing mode");
        s_pairing_mode = false;
        if (s_pairing_timer != NULL) {
            xTimerStop(s_pairing_timer, 0);
        }
    }

    if (s_wifi_mode) {
        // Switch to Zigbee mode
        ESP_LOGI(TAG, "Switching to Zigbee automation mode");
        wifi_task_stop();

        // Start Zigbee task if not already started
        if (!s_zigbee_started) {
            ESP_LOGI(TAG, "Starting Zigbee task for the first time");
            zigbee_task_start();
            s_zigbee_started = true;
        } else {
            // Resume Zigbee radio if it was previously stopped
            ESP_LOGI(TAG, "Resuming Zigbee radio");
            zigbee_task_resume();
        }

        scheduler_task_start();
        s_wifi_mode = false;

        xEventGroupSetBits(g_event_group, EVENT_ZIGBEE_MODE_BIT);
        xEventGroupClearBits(g_event_group, EVENT_WIFI_MODE_BIT);

        // Set LED state to normal (automation mode)
        led_set_state(LED_STATE_NORMAL);
    } else {
        // Switch to Wi-Fi mode
        // NOTE: On ESP32-C6, Wi-Fi and Zigbee share the radio
        ESP_LOGI(TAG, "Switching to Wi-Fi configuration mode");
        scheduler_task_stop();

        // Stop Zigbee radio to free up the shared radio for Wi-Fi
        if (s_zigbee_started) {
            zigbee_task_stop();
        }

        // Give the system time to settle before starting Wi-Fi
        // This helps with radio coexistence on ESP32-C6
        vTaskDelay(pdMS_TO_TICKS(500));

        esp_err_t ret = wifi_task_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Wi-Fi, retrying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            ret = wifi_task_start();
        }

        if (ret == ESP_OK) {
            s_wifi_mode = true;
            xEventGroupSetBits(g_event_group, EVENT_WIFI_MODE_BIT);
            xEventGroupClearBits(g_event_group, EVENT_ZIGBEE_MODE_BIT);
            led_set_state(LED_STATE_WIFI_ACTIVE);
        } else {
            ESP_LOGE(TAG, "Wi-Fi start failed after retry");
            led_set_state(LED_STATE_ERROR);
        }
    }
}

static void on_button_long_press(void)
{
    ESP_LOGI(TAG, "Long press - entering Zigbee pairing mode");

    // Zigbee must be running for pairing
    if (!s_zigbee_started) {
        ESP_LOGW(TAG, "Zigbee not started - switch to automation mode first");
        // Flash error indication
        led_set_state(LED_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_wifi_mode) {
            led_set_state(LED_STATE_WIFI_ACTIVE);
        } else {
            led_set_state(LED_STATE_NORMAL);
        }
        return;
    }

    // Enable permit join for 60 seconds
    esp_err_t ret = zigbee_permit_join(ZIGBEE_PERMIT_JOIN_TIME);
    if (ret == ESP_OK) {
        s_pairing_mode = true;
        led_set_state(LED_STATE_PAIRING);
        ESP_LOGI(TAG, "Zigbee pairing enabled for %d seconds", ZIGBEE_PERMIT_JOIN_TIME);

        // Start timer to auto-disable pairing mode
        if (s_pairing_timer == NULL) {
            s_pairing_timer = xTimerCreate(
                "pairing_timer",
                pdMS_TO_TICKS(ZIGBEE_PERMIT_JOIN_TIME * 1000),
                pdFALSE,  // One-shot timer
                NULL,
                pairing_timer_callback
            );
        }
        if (s_pairing_timer != NULL) {
            xTimerStart(s_pairing_timer, 0);
        }
    } else {
        ESP_LOGE(TAG, "Failed to enable Zigbee pairing: %s", esp_err_to_name(ret));
        led_set_state(LED_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_wifi_mode) {
            led_set_state(LED_STATE_WIFI_ACTIVE);
        } else {
            led_set_state(LED_STATE_NORMAL);
        }
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
    ESP_ERROR_CHECK(button_task_init(on_button_short_press, on_button_long_press));

    // Initialize Wi-Fi task
    ESP_ERROR_CHECK(wifi_task_init());

    // Initialize Zigbee task (but don't start yet - radio shared with Wi-Fi)
    ESP_ERROR_CHECK(zigbee_task_init());

    // Initialize scheduler task
    ESP_ERROR_CHECK(scheduler_task_init());

    // Start in Wi-Fi AP mode (as per user preference)
    // NOTE: Zigbee task NOT started here - Wi-Fi and Zigbee share the same radio on ESP32-C6
    // Zigbee will start when user switches to Zigbee mode via button press
    ESP_LOGI(TAG, "Starting in Wi-Fi AP mode (Zigbee will start when mode switched)");
    ESP_ERROR_CHECK(wifi_task_start());
    s_wifi_mode = true;

    xEventGroupSetBits(g_event_group, EVENT_WIFI_MODE_BIT);
    led_set_state(LED_STATE_WIFI_ACTIVE);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "System initialization complete");
    ESP_LOGI(TAG, "Wi-Fi SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "Wi-Fi Password: %s", WIFI_PASSWORD);
    ESP_LOGI(TAG, "Web interface: http://%s", WIFI_AP_IP);
    ESP_LOGI(TAG, "Button GPIO %d: short press=toggle mode, long press=pairing", GPIO_BUTTON_TOGGLE);
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
