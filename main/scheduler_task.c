/**
 * @file scheduler_task.c
 * @brief Automation scheduler task implementation
 */

#include "scheduler_task.h"
#include "device_manager.h"
#include "wifi_task.h"
#include "led_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <time.h>

static const char *TAG = "SCHEDULER_TASK";

// External declarations
extern EventGroupHandle_t g_event_group;
extern QueueHandle_t g_cmd_queue;

static TaskHandle_t s_scheduler_task_handle = NULL;
static bool s_scheduler_active = false;
static uint32_t s_delay_cycle_start = 0;
static bool s_last_delay_state = false;  // Track last sent delay state to detect transitions
static bool s_delay_first_run = true;    // Force first command on startup

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

// ============================================================================
// Time Comparison Helpers
// ============================================================================

static bool time_matches(time_point_t *tp, struct tm *current_time)
{
    return (tp->hour == current_time->tm_hour && tp->minute == current_time->tm_min);
}

static uint32_t get_minutes_since_cycle_start(void)
{
    if (s_delay_cycle_start == 0) {
        return 0;
    }

    time_t now = time(NULL);
    uint32_t elapsed_seconds = (uint32_t)(now - s_delay_cycle_start);
    return elapsed_seconds / 60;
}

// ============================================================================
// Fixed Time Mode Processing
// ============================================================================

static void process_fixed_time_mode(device_config_t *dev, struct tm *current_time)
{
    if (!dev->enabled) {
        return;
    }

    for (int i = 0; i < dev->time_pair_count; i++) {
        time_pair_t *pair = &dev->time_pairs[i];

        // Check ON time - always send command regardless of saved state
        // (device state can change via physical switch without our knowledge)
        if (time_matches(&pair->on_time, current_time)) {
            char ieee_str[24];
            format_ieee_addr_str(ieee_str, sizeof(ieee_str), dev->ieee_addr);
            ESP_LOGI(TAG, "Fixed time ON triggered for device %s", ieee_str);

            cmd_queue_msg_t msg = {
                .ieee_addr = dev->ieee_addr,
                .endpoint = dev->endpoint,
                .cmd = CMD_ON
            };
            xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));
        }

        // Check OFF time - always send command regardless of saved state
        if (time_matches(&pair->off_time, current_time)) {
            char ieee_str[24];
            format_ieee_addr_str(ieee_str, sizeof(ieee_str), dev->ieee_addr);
            ESP_LOGI(TAG, "Fixed time OFF triggered for device %s", ieee_str);

            cmd_queue_msg_t msg = {
                .ieee_addr = dev->ieee_addr,
                .endpoint = dev->endpoint,
                .cmd = CMD_OFF
            };
            xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));
        }
    }
}

// ============================================================================
// Delay Mode Processing
// ============================================================================

static void process_delay_mode(device_config_t *dev)
{
    if (!dev->enabled) {
        return;
    }
    if (s_delay_cycle_start == 0) {
        return;
    }

    uint32_t minutes_elapsed = get_minutes_since_cycle_start();
    uint32_t cycle_length = dev->delay_on_minutes + dev->delay_duration_minutes;

    if (cycle_length == 0) {
        return;
    }

    // Calculate position within current cycle
    uint32_t position_in_cycle = minutes_elapsed % cycle_length;

    // Determine expected state
    // 0 to delay_on_minutes-1: OFF (waiting)
    // delay_on_minutes to cycle_length-1: ON (active)
    bool expected_state = (position_in_cycle >= dev->delay_on_minutes);

    // Send command when the expected state changes (based on cycle position, not device state)
    // This ensures commands are sent even if device state was changed manually
    // Also send on first run after scheduler start/resume
    if (expected_state != s_last_delay_state || s_delay_first_run) {
        s_delay_first_run = false;
        char ieee_str[24];
        format_ieee_addr_str(ieee_str, sizeof(ieee_str), dev->ieee_addr);

        // Calculate time until next state change for logging
        uint32_t minutes_until_change;
        if (expected_state) {
            minutes_until_change = cycle_length - position_in_cycle;
        } else {
            minutes_until_change = dev->delay_on_minutes - position_in_cycle;
        }

        ESP_LOGI(TAG, "Delay mode %s triggered for %s (pos: %lu/%lu min, next change in %lu min)",
                 expected_state ? "ON" : "OFF",
                 ieee_str,
                 (unsigned long)position_in_cycle,
                 (unsigned long)cycle_length,
                 (unsigned long)minutes_until_change);

        cmd_queue_msg_t msg = {
            .ieee_addr = dev->ieee_addr,
            .endpoint = dev->endpoint,
            .cmd = expected_state ? CMD_ON : CMD_OFF
        };
        xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));

        // Track what we sent (not the device state)
        s_last_delay_state = expected_state;
    }
}

// ============================================================================
// Scheduler Task
// ============================================================================

static void scheduler_task(void *pvParameters)
{
    struct tm current_time;
    uint8_t last_minute = 255;

    ESP_LOGI(TAG, "Scheduler task started");

    for (;;) {
        // Wait for scheduler to be active and RTC to be initialized
        EventBits_t bits = xEventGroupWaitBits(
            g_event_group,
            EVENT_ZIGBEE_MODE_BIT | EVENT_RTC_INIT_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(1000)
        );

        if (!(bits & EVENT_ZIGBEE_MODE_BIT) || !(bits & EVENT_RTC_INIT_BIT)) {
            continue;
        }

        if (!s_scheduler_active) {
            continue;
        }

        // Get current time
        time_t now = time(NULL);
        localtime_r(&now, &current_time);

        // Process once per minute for fixed time mode
        if (current_time.tm_min != last_minute) {
            last_minute = current_time.tm_min;

            ESP_LOGD(TAG, "Scheduler tick: %02d:%02d:%02d",
                     current_time.tm_hour, current_time.tm_min, current_time.tm_sec);

            // Process each device
            uint8_t count = device_manager_get_count();
            for (uint8_t i = 0; i < count; i++) {
                device_config_t dev;
                if (device_manager_get_by_index(i, &dev) != ESP_OK) {
                    continue;
                }

                if (dev.mode == MODE_FIXED_TIME) {
                    process_fixed_time_mode(&dev, &current_time);
                }
            }
        }

        // Process delay mode more frequently (every 10 seconds)
        uint8_t count = device_manager_get_count();
        for (uint8_t i = 0; i < count; i++) {
            device_config_t dev;
            if (device_manager_get_by_index(i, &dev) != ESP_OK) {
                continue;
            }

            if (dev.mode == MODE_DELAY) {
                process_delay_mode(&dev);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10000));  // 10 second interval for delay mode
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t scheduler_task_init(void)
{
    ESP_LOGI(TAG, "Scheduler task initialized");
    return ESP_OK;
}

esp_err_t scheduler_task_start(void)
{
    // Check if task already exists - just resume it
    if (s_scheduler_task_handle != NULL) {
        s_scheduler_active = true;
        s_delay_cycle_start = (uint32_t)time(NULL);
        s_delay_first_run = true;  // Force first command on resume
        ESP_LOGI(TAG, "Scheduler task resumed, delay cycle reset");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(
        scheduler_task,
        "scheduler_task",
        TASK_STACK_SCHEDULER,
        NULL,
        TASK_PRIORITY_SCHEDULER,
        &s_scheduler_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scheduler task");
        return ESP_FAIL;
    }

    s_scheduler_active = true;
    // Initialize delay cycle start time
    s_delay_cycle_start = (uint32_t)time(NULL);
    s_delay_first_run = true;  // Force first command on start
    ESP_LOGI(TAG, "Scheduler task started, delay cycle initialized at %lu", (unsigned long)s_delay_cycle_start);
    return ESP_OK;
}

void scheduler_task_stop(void)
{
    s_scheduler_active = false;
    ESP_LOGI(TAG, "Scheduler task stopped");
}

void scheduler_task_resume(void)
{
    s_scheduler_active = true;

    // Reset delay cycle start time
    s_delay_cycle_start = (uint32_t)time(NULL);
    s_delay_first_run = true;  // Force first command on resume

    ESP_LOGI(TAG, "Scheduler task resumed, delay cycle reset");
}

void scheduler_reset_delay_cycles(void)
{
    s_delay_cycle_start = (uint32_t)time(NULL);
    s_delay_first_run = true;  // Force first command after reset
    ESP_LOGI(TAG, "Delay cycles reset at timestamp %lu", (unsigned long)s_delay_cycle_start);
}
