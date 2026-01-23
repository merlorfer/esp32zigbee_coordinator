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
#include "ble_task.h"
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

static bool s_wifi_mode = true;   // Track if WiFi is active
static bool s_ble_mode = true;    // Track if BLE is active
static bool s_setup_mode = true;  // Track if in initial setup phase
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
    ESP_LOGI(TAG, "Short press detected");

    // If in pairing mode, cancel it first
    if (s_pairing_mode) {
        ESP_LOGI(TAG, "Cancelling pairing mode");
        s_pairing_mode = false;
        if (s_pairing_timer != NULL) {
            xTimerStop(s_pairing_timer, 0);
        }
    }

    if (s_setup_mode) {
        // Setup mode → Operational mode (WiFi+BLE → Zigbee)
        ESP_LOGI(TAG, "Exiting setup mode, starting Zigbee operational mode");

        wifi_task_stop();
        ble_task_stop();
        s_wifi_mode = false;
        s_ble_mode = false;
        s_setup_mode = false;

        // Clear any old commands in the queue
        cmd_queue_msg_t dummy_msg;
        while (xQueueReceive(g_cmd_queue, &dummy_msg, 0) == pdTRUE) {
            ESP_LOGD(TAG, "Cleared old command from queue");
        }

        // Start Zigbee task
        if (!s_zigbee_started) {
            ESP_LOGI(TAG, "Starting Zigbee task for the first time");
            zigbee_task_start();
            s_zigbee_started = true;

            // Give time for Zigbee network formation
            ESP_LOGI(TAG, "Waiting for Zigbee network formation...");
            vTaskDelay(pdMS_TO_TICKS(8000));

            // Process any pending leave requests
            wifi_task_process_pending_leave();
        }

        scheduler_task_start();

        led_set_state(LED_STATE_NORMAL);
        xEventGroupSetBits(g_event_group, EVENT_ZIGBEE_MODE_BIT);
        xEventGroupClearBits(g_event_group, EVENT_WIFI_MODE_BIT | EVENT_BLE_MODE_BIT);

        ESP_LOGI(TAG, "Now in Zigbee operational mode");
    } else if (!s_ble_mode) {
        // Zigbee only → Zigbee + BLE config mode
        ESP_LOGI(TAG, "Starting BLE configuration mode");
        ble_task_start();
        s_ble_mode = true;
        led_set_state(LED_STATE_BLE_ACTIVE);
        xEventGroupSetBits(g_event_group, EVENT_BLE_MODE_BIT);
        ESP_LOGI(TAG, "BLE configuration mode active");
    } else {
        // Zigbee + BLE → Zigbee only mode
        ESP_LOGI(TAG, "Stopping BLE, returning to automation mode");
        ble_task_stop();
        s_ble_mode = false;
        led_set_state(LED_STATE_NORMAL);
        xEventGroupClearBits(g_event_group, EVENT_BLE_MODE_BIT);
        ESP_LOGI(TAG, "Back to Zigbee automation mode");
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

    // NOTE: Wi-Fi and Zigbee coexistence is configured in menuconfig:
    //   Component config -> Wi-Fi -> Software controls WiFi/Bluetooth coexistence -> [*]
    //   Component config -> ESP System Settings -> Event loop task stack size -> 4096
    // The coexistence is automatic when both Wi-Fi and Zigbee are running

    // Initialize Wi-Fi task
    ESP_ERROR_CHECK(wifi_task_init());

    // Initialize BLE task
    ESP_ERROR_CHECK(ble_task_init());

    // Initialize Zigbee task (but don't start yet)
    ESP_ERROR_CHECK(zigbee_task_init());

    // Initialize scheduler task
    ESP_ERROR_CHECK(scheduler_task_init());

    // Start in setup mode with WiFi AP + BLE
    ESP_LOGI(TAG, "Starting in setup mode (WiFi AP + BLE)");
    ESP_ERROR_CHECK(wifi_task_start());
    ESP_ERROR_CHECK(ble_task_start());

    s_wifi_mode = true;
    s_ble_mode = true;
    s_setup_mode = true;

    xEventGroupSetBits(g_event_group, EVENT_WIFI_MODE_BIT | EVENT_BLE_MODE_BIT);
    led_set_state(LED_STATE_WIFI_ACTIVE);  // WiFi indicator for setup

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "System initialization complete");
    ESP_LOGI(TAG, "Setup mode active (WiFi + BLE)");
    ESP_LOGI(TAG, "Wi-Fi SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "Wi-Fi Password: %s", WIFI_PASSWORD);
    ESP_LOGI(TAG, "Web interface: http://%s", WIFI_AP_IP);
    ESP_LOGI(TAG, "BLE device: ESP32C6_Gateway");
    ESP_LOGI(TAG, "Button: short press=mode switch, long press=pairing");
    ESP_LOGI(TAG, "========================================");

    // Main task can now idle - all work is done in other tasks
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        // Log system status periodically
        ESP_LOGD(TAG, "System running - WiFi:%s, BLE:%s, RTC:%s, Devices:%d",
                 s_wifi_mode ? "ON" : "OFF",
                 s_ble_mode ? "ON" : "OFF",
                 wifi_task_is_rtc_initialized() || ble_task_is_rtc_initialized() ? "SET" : "NOT SET",
                 device_manager_get_count());
    }
}
