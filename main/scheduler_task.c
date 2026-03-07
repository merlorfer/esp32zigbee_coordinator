/**
 * @file scheduler_task.c
 * @brief Automation scheduler task implementation
 */

#include "scheduler_task.h"
#include "device_manager.h"
#include "sensor_types.h"
#include "rules_engine.h"
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
extern QueueHandle_t g_sensor_data_queue;

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
// Sensor Actuator Check
// ============================================================================

/**
 * @brief Check if an ON/OFF device is being used as an actuator by any sensor.
 *        If so, its own time/delay automation should be skipped to avoid conflicts.
 */
static bool is_sensor_actuator(uint64_t ieee_addr)
{
    uint8_t count = device_manager_get_count();
    for (uint8_t i = 0; i < count; i++) {
        device_config_t dev;
        if (device_manager_get_by_index(i, &dev) != ESP_OK) continue;

        if (!is_sensor_device(dev.device_type)) continue;

        if (dev.sensor.lower_linked_device == ieee_addr ||
            dev.sensor.upper_linked_device == ieee_addr ||
            dev.sensor.error_linked_device == ieee_addr) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Fixed Time Mode Processing
// ============================================================================

static void process_fixed_time_mode(device_config_t *dev, struct tm *current_time)
{
    if (!dev->enabled) {
        return;
    }

    // Skip if this device is controlled by a sensor — avoid conflicting commands
    if (is_sensor_actuator(dev->ieee_addr)) {
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

    // Skip if this device is controlled by a sensor — avoid conflicting commands
    if (is_sensor_actuator(dev->ieee_addr)) {
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
// Sensor Processing Functions
// ============================================================================

/**
 * @brief Process sensor data from the queue and update device readings
 */
static void process_sensor_data(void)
{
    sensor_data_msg_t sensor_msg;

    // Process all queued sensor data
    while (xQueueReceive(g_sensor_data_queue, &sensor_msg, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Processing sensor data from queue - cluster: 0x%04x", sensor_msg.cluster_id);
        // Convert raw value to actual reading using sensor type registry
        const sensor_type_info_t *info = sensor_type_get_by_cluster(sensor_msg.cluster_id);
        if (info == NULL) {
            ESP_LOGW(TAG, "Unknown cluster ID: 0x%04x", sensor_msg.cluster_id);
            continue;
        }

        float converted_value = (float)sensor_msg.raw_value / info->conversion_divisor;
        device_type_t device_type = info->device_type;
        ESP_LOGI(TAG, "%s sensor data: %.2f%s (raw: %d)",
                 info->display_name, converted_value, info->unit, sensor_msg.raw_value);

        // Update device reading
        esp_err_t ret = device_manager_update_sensor_reading(
            sensor_msg.ieee_addr,
            sensor_msg.endpoint,
            sensor_msg.raw_value,
            converted_value,
            device_type
        );

        if (ret != ESP_OK) {
            // Device not found - this can happen during initial pairing when data arrives
            // before the device is fully added. Just skip silently.
            ESP_LOGD(TAG, "Sensor device not found yet (normal during pairing)");
        } else {
            // Notify rules engine about sensor data
            int dev_idx = device_manager_find_index_by_type(sensor_msg.ieee_addr, device_type);
            if (dev_idx >= 0) {
                rules_engine_on_sensor_data((uint8_t)dev_idx, converted_value);
            }
        }
    }
}

/**
 * @brief Evaluate sensor thresholds and send ON/OFF commands to linked devices
 */
static void process_sensor_thresholds(void)
{
    uint8_t count = device_manager_get_count();
    time_t now = time(NULL);

    for (uint8_t i = 0; i < count; i++) {
        device_config_t dev;
        if (device_manager_get_by_index(i, &dev) != ESP_OK) {
            continue;
        }

        // Only process sensor devices
        if (!is_sensor_device(dev.device_type)) {
            continue;
        }

        // Skip if reading is invalid
        if (!dev.reading.valid) {
            continue;
        }

        float value = dev.reading.converted_value;

        // ===== LOWER THRESHOLD (Heating) =====
        if (dev.sensor.lower_linked_device != 0) {
            // Debounce: minimum 10 seconds between commands
            if (now - dev.sensor.last_lower_action >= 10) {
                if (!dev.sensor.lower_active && !dev.sensor.lower_delay_pending
                    && value < dev.sensor.lower_threshold) {
                    // Threshold crossed
                    if (dev.sensor.lower_delay_seconds == 0) {
                        // No delay - immediate ON (original behavior)
                        ESP_LOGI(TAG, "Lower threshold triggered: %.2f < %.2f - Turning ON linked device",
                                 value, dev.sensor.lower_threshold);

                        cmd_queue_msg_t msg = {
                            .ieee_addr = dev.sensor.lower_linked_device,
                            .endpoint = 1,
                            .cmd = CMD_ON
                        };
                        xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));

                        dev.sensor.lower_active = true;
                        dev.sensor.last_lower_action = now;
                        device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                    } else {
                        // Start delay timer
                        dev.sensor.lower_delay_pending = true;
                        dev.sensor.lower_delay_start = (uint32_t)now;
                        ESP_LOGI(TAG, "Lower threshold crossed: %.2f < %.2f - delay started: %d sec",
                                 value, dev.sensor.lower_threshold, dev.sensor.lower_delay_seconds);
                        device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                    }
                }
                else if (dev.sensor.lower_delay_pending) {
                    if (value >= dev.sensor.lower_threshold) {
                        // Value recovered during delay - cancel
                        dev.sensor.lower_delay_pending = false;
                        dev.sensor.lower_delay_start = 0;
                        ESP_LOGI(TAG, "Lower delay cancelled - value recovered: %.2f >= %.2f",
                                 value, dev.sensor.lower_threshold);
                        device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                    }
                    else if ((now - dev.sensor.lower_delay_start) >= dev.sensor.lower_delay_seconds) {
                        // Delay expired - check again and activate
                        if (value < dev.sensor.lower_threshold) {
                            ESP_LOGI(TAG, "Lower delay expired, still below threshold: %.2f < %.2f - Turning ON",
                                     value, dev.sensor.lower_threshold);

                            cmd_queue_msg_t msg = {
                                .ieee_addr = dev.sensor.lower_linked_device,
                                .endpoint = 1,
                                .cmd = CMD_ON
                            };
                            xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));

                            dev.sensor.lower_active = true;
                            dev.sensor.last_lower_action = now;
                        }
                        dev.sensor.lower_delay_pending = false;
                        dev.sensor.lower_delay_start = 0;
                        device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                    }
                    // else: delay still running, do nothing
                }
                else if (dev.sensor.lower_active
                         && value > (dev.sensor.lower_threshold + dev.sensor.lower_hysteresis)) {
                    // Turn OFF heating - immediate (no delay for OFF)
                    ESP_LOGI(TAG, "Lower threshold + hysteresis cleared: %.2f > %.2f - Turning OFF linked device",
                             value, dev.sensor.lower_threshold + dev.sensor.lower_hysteresis);

                    cmd_queue_msg_t msg = {
                        .ieee_addr = dev.sensor.lower_linked_device,
                        .endpoint = 1,
                        .cmd = CMD_OFF
                    };
                    xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));

                    dev.sensor.lower_active = false;
                    dev.sensor.last_lower_action = now;
                    device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                }
            }
        }

        // ===== UPPER THRESHOLD (Cooling) =====
        if (dev.sensor.upper_linked_device != 0) {
            // Debounce: minimum 10 seconds between commands
            if (now - dev.sensor.last_upper_action >= 10) {
                if (!dev.sensor.upper_active && !dev.sensor.upper_delay_pending
                    && value > dev.sensor.upper_threshold) {
                    // Threshold crossed
                    if (dev.sensor.upper_delay_seconds == 0) {
                        // No delay - immediate ON (original behavior)
                        ESP_LOGI(TAG, "Upper threshold triggered: %.2f > %.2f - Turning ON linked device",
                                 value, dev.sensor.upper_threshold);

                        cmd_queue_msg_t msg = {
                            .ieee_addr = dev.sensor.upper_linked_device,
                            .endpoint = 1,
                            .cmd = CMD_ON
                        };
                        xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));

                        dev.sensor.upper_active = true;
                        dev.sensor.last_upper_action = now;
                        device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                    } else {
                        // Start delay timer
                        dev.sensor.upper_delay_pending = true;
                        dev.sensor.upper_delay_start = (uint32_t)now;
                        ESP_LOGI(TAG, "Upper threshold crossed: %.2f > %.2f - delay started: %d sec",
                                 value, dev.sensor.upper_threshold, dev.sensor.upper_delay_seconds);
                        device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                    }
                }
                else if (dev.sensor.upper_delay_pending) {
                    if (value <= dev.sensor.upper_threshold) {
                        // Value recovered during delay - cancel
                        dev.sensor.upper_delay_pending = false;
                        dev.sensor.upper_delay_start = 0;
                        ESP_LOGI(TAG, "Upper delay cancelled - value recovered: %.2f <= %.2f",
                                 value, dev.sensor.upper_threshold);
                        device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                    }
                    else if ((now - dev.sensor.upper_delay_start) >= dev.sensor.upper_delay_seconds) {
                        // Delay expired - check again and activate
                        if (value > dev.sensor.upper_threshold) {
                            ESP_LOGI(TAG, "Upper delay expired, still above threshold: %.2f > %.2f - Turning ON",
                                     value, dev.sensor.upper_threshold);

                            cmd_queue_msg_t msg = {
                                .ieee_addr = dev.sensor.upper_linked_device,
                                .endpoint = 1,
                                .cmd = CMD_ON
                            };
                            xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));

                            dev.sensor.upper_active = true;
                            dev.sensor.last_upper_action = now;
                        }
                        dev.sensor.upper_delay_pending = false;
                        dev.sensor.upper_delay_start = 0;
                        device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                    }
                    // else: delay still running, do nothing
                }
                else if (dev.sensor.upper_active
                         && value < (dev.sensor.upper_threshold - dev.sensor.upper_hysteresis)) {
                    // Turn OFF cooling - immediate (no delay for OFF)
                    ESP_LOGI(TAG, "Upper threshold - hysteresis cleared: %.2f < %.2f - Turning OFF linked device",
                             value, dev.sensor.upper_threshold - dev.sensor.upper_hysteresis);

                    cmd_queue_msg_t msg = {
                        .ieee_addr = dev.sensor.upper_linked_device,
                        .endpoint = 1,
                        .cmd = CMD_OFF
                    };
                    xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));

                    dev.sensor.upper_active = false;
                    dev.sensor.last_upper_action = now;
                    device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
                }
            }
        }
    }
}

/**
 * @brief Monitor sensor timeouts and turn off linked devices if no data
 */
static void monitor_sensor_timeouts(void)
{
    uint8_t count = device_manager_get_count();
    time_t now = time(NULL);

    for (uint8_t i = 0; i < count; i++) {
        device_config_t dev;
        if (device_manager_get_by_index(i, &dev) != ESP_OK) {
            continue;
        }

        // Only process sensor devices
        if (!is_sensor_device(dev.device_type)) {
            continue;
        }

        // Skip if reading was never valid
        if (dev.reading.last_update == 0) {
            continue;
        }

        // timeout_seconds == 0 means disabled (event-driven sensors like IAS Zone)
        if (dev.sensor.timeout_seconds == 0) {
            continue;
        }

        // Check timeout
        uint32_t elapsed = now - dev.reading.last_update;
        if (elapsed > dev.sensor.timeout_seconds) {
            // Sensor timeout occurred
            if (dev.reading.valid) {
                char ieee_str[24];
                format_ieee_addr_str(ieee_str, sizeof(ieee_str), dev.ieee_addr);
                ESP_LOGW(TAG, "Sensor timeout for %s (no data for %lu seconds)", ieee_str, (unsigned long)elapsed);

                // Turn OFF linked devices
                if (dev.sensor.lower_linked_device != 0) {
                    cmd_queue_msg_t msg = {
                        .ieee_addr = dev.sensor.lower_linked_device,
                        .endpoint = 1,
                        .cmd = CMD_OFF
                    };
                    xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));
                    ESP_LOGI(TAG, "Turned OFF lower linked device due to timeout");
                }

                if (dev.sensor.upper_linked_device != 0) {
                    cmd_queue_msg_t msg = {
                        .ieee_addr = dev.sensor.upper_linked_device,
                        .endpoint = 1,
                        .cmd = CMD_OFF
                    };
                    xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));
                    ESP_LOGI(TAG, "Turned OFF upper linked device due to timeout");
                }

                // Execute error action on error-linked device
                if (dev.sensor.error_linked_device != 0) {
                    cmd_queue_msg_t msg = {
                        .ieee_addr = dev.sensor.error_linked_device,
                        .endpoint = 1,
                        .cmd = dev.sensor.error_action_on ? CMD_ON : CMD_OFF
                    };
                    xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));
                    ESP_LOGI(TAG, "Timeout error action: %s error linked device",
                             dev.sensor.error_action_on ? "Turned ON" : "Turned OFF");
                }

                // Clear any pending delay timers
                dev.sensor.lower_delay_pending = false;
                dev.sensor.lower_delay_start = 0;
                dev.sensor.upper_delay_pending = false;
                dev.sensor.upper_delay_start = 0;

                // Set error and invalidate reading
                device_manager_set_error(dev.ieee_addr, "Sensor timeout");
                dev.reading.valid = false;
                device_manager_update_by_type(dev.ieee_addr, dev.device_type, &dev);
            }
        }
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
            // Even when scheduler is paused (BLE mode), keep draining the sensor data queue
            if (g_sensor_data_queue != NULL) {
                process_sensor_data();
            }
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

        // Process sensor data queue
        if (g_sensor_data_queue != NULL) {
            process_sensor_data();
        }

        // Evaluate sensor thresholds
        process_sensor_thresholds();

        // Rules engine: timer countdown and time-based triggers
        rules_engine_timer_tick();
        rules_engine_on_time_tick(current_time.tm_hour, current_time.tm_min);

        // Monitor sensor timeouts
        monitor_sensor_timeouts();

        vTaskDelay(pdMS_TO_TICKS(10000));  // 10 second interval for delay mode and sensor processing
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t scheduler_task_init(void)
{
    rules_engine_init();
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

    // Initialize and trigger rules engine boot event
    rules_engine_init();
    rules_engine_on_boot();

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
