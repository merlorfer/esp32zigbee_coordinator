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

// Per-device phase tracking structure
typedef struct {
    uint64_t ieee_addr;
    uint8_t last_phase;      // Track last phase (0=OFF1, 1=ON, 2=OFF2, 255=uninitialized)
    bool last_state;         // Track last sent state
} device_phase_tracking_t;

#define MAX_TRACKED_DEVICES 10
static device_phase_tracking_t s_device_phases[MAX_TRACKED_DEVICES] = {0};
static uint8_t s_tracked_device_count = 0;

static TaskHandle_t s_scheduler_task_handle = NULL;
static bool s_scheduler_active = false;
static uint32_t s_delay_cycle_start = 0;

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

/* Get or create phase tracking for a device */
static device_phase_tracking_t* get_device_phase_tracking(uint64_t ieee_addr)
{
    // Search for existing entry
    for (uint8_t i = 0; i < s_tracked_device_count; i++) {
        if (s_device_phases[i].ieee_addr == ieee_addr) {
            return &s_device_phases[i];
        }
    }

    // Create new entry if space available
    if (s_tracked_device_count < MAX_TRACKED_DEVICES) {
        device_phase_tracking_t *new_entry = &s_device_phases[s_tracked_device_count];
        new_entry->ieee_addr = ieee_addr;
        new_entry->last_phase = 255;  // Uninitialized
        new_entry->last_state = false;
        s_tracked_device_count++;
        return new_entry;
    }

    // No space available, log error
    ESP_LOGE(TAG, "Phase tracking full, cannot track more devices");
    return NULL;
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

    // Get phase tracking for this device
    device_phase_tracking_t *tracking = get_device_phase_tracking(dev->ieee_addr);
    if (tracking == NULL) {
        return;  // Cannot track this device
    }

    uint32_t minutes_elapsed = get_minutes_since_cycle_start();

    // 3-phase cycle: OFF1 → ON → OFF2
    uint32_t cycle_length = dev->delay_off1_minutes + dev->delay_duration_minutes + dev->delay_off2_minutes;

    if (cycle_length == 0) {
        return;
    }

    // Calculate position within current cycle
    uint32_t position_in_cycle = minutes_elapsed % cycle_length;

    // Determine expected state based on 3-phase cycle
    // Phase 1 (OFF1): 0 <= position < off1
    // Phase 2 (ON):   off1 <= position < (off1 + duration)
    // Phase 3 (OFF2): (off1 + duration) <= position < cycle_length
    uint32_t on_start = dev->delay_off1_minutes;
    uint32_t on_end = dev->delay_off1_minutes + dev->delay_duration_minutes;

    // Determine current phase (0=OFF1, 1=ON, 2=OFF2)
    uint8_t current_phase;
    const char *phase_name;
    uint32_t minutes_until_change;

    if (position_in_cycle < on_start) {
        // Phase 1: OFF1
        current_phase = 0;
        phase_name = "OFF1";
        minutes_until_change = on_start - position_in_cycle;
    } else if (position_in_cycle < on_end) {
        // Phase 2: ON
        current_phase = 1;
        phase_name = "ON";
        minutes_until_change = on_end - position_in_cycle;
    } else {
        // Phase 3: OFF2
        current_phase = 2;
        phase_name = "OFF2";
        minutes_until_change = cycle_length - position_in_cycle;
    }

    bool expected_state = (current_phase == 1);  // ON only in phase 1

    // Send command when the phase changes or device is uninitialized (last_phase == 255)
    // This ensures OFF1 → OFF2 transitions are also handled
    // Use per-device phase tracking instead of global variables
    if (current_phase != tracking->last_phase) {
        char ieee_str[24];
        format_ieee_addr_str(ieee_str, sizeof(ieee_str), dev->ieee_addr);

        ESP_LOGI(TAG, "Delay mode %s triggered for %s (phase: %s, pos: %lu/%lu min, next change in %lu min)",
                 expected_state ? "ON" : "OFF",
                 ieee_str,
                 phase_name,
                 (unsigned long)position_in_cycle,
                 (unsigned long)cycle_length,
                 (unsigned long)minutes_until_change);

        cmd_queue_msg_t msg = {
            .ieee_addr = dev->ieee_addr,
            .endpoint = dev->endpoint,
            .cmd = expected_state ? CMD_ON : CMD_OFF
        };
        xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));

        // Track what we sent for THIS device (both state and phase)
        tracking->last_state = expected_state;
        tracking->last_phase = current_phase;
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

        if (!(bits & EVENT_ZIGBEE_MODE_BIT)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!(bits & EVENT_RTC_INIT_BIT)) {
            ESP_LOGW(TAG, "RTC not initialized - time-based scheduling disabled");
            vTaskDelay(pdMS_TO_TICKS(10000)); // Wait 10 sec before checking again
            continue;
        }

        if (!s_scheduler_active) {
            vTaskDelay(pdMS_TO_TICKS(1000));
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
        // Wall clock method: Do not reset the cycle start on resume!
        // Only set it if it's still 0 (e.g., very first start or error)
        if (s_delay_cycle_start == 0) {
            s_delay_cycle_start = (uint32_t)time(NULL);
        }
        ESP_LOGI(TAG, "Scheduler task resumed, delay cycle continues (start: %lu)", (unsigned long)s_delay_cycle_start);
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
    // Reset all device phase tracking
    for (uint8_t i = 0; i < s_tracked_device_count; i++) {
        s_device_phases[i].last_phase = 255;
    }
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

    // Wall clock method: Do not reset the cycle start on resume!
    if (s_delay_cycle_start == 0) {
        s_delay_cycle_start = (uint32_t)time(NULL);
    }

    // Reset all device phase tracking to force immediate command on resume
    for (uint8_t i = 0; i < s_tracked_device_count; i++) {
        s_device_phases[i].last_phase = 255;
    }

    ESP_LOGI(TAG, "Scheduler task resumed, delay cycle continues (start: %lu)", (unsigned long)s_delay_cycle_start);
}

void scheduler_reset_delay_cycles(void)
{
    s_delay_cycle_start = (uint32_t)time(NULL);
    // Reset all device phase tracking to force immediate command after reset
    for (uint8_t i = 0; i < s_tracked_device_count; i++) {
        s_device_phases[i].last_phase = 255;
    }
    ESP_LOGI(TAG, "Delay cycles reset at timestamp %lu", (unsigned long)s_delay_cycle_start);
}
