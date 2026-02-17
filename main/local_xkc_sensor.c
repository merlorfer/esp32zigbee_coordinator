/**
 * @file local_xkc_sensor.c
 * @brief Local XKC water level sensor implementation
 *
 * Uses esp_timer (no new FreeRTOS task) to periodically read two XKC
 * non-contact water level sensors via GPIO. Data is fed through the
 * existing g_sensor_data_queue so the scheduler handles thresholds,
 * timeouts, and linked device automation automatically.
 */

#include "local_xkc_sensor.h"
#include "common.h"
#include "device_manager.h"
#include "sensor_types.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>
#include <time.h>

static const char *TAG = "LOCAL_XKC";

/* Valid GPIO pins for XKC sensors on ESP32-C6 */
static const uint8_t s_valid_gpios[] = {
    3, 4, 5, 6, 7, 10, 11, 14, 18, 19, 20, 21, 22, 23
};

/* Module state */
static esp_timer_handle_t s_read_timer = NULL;
static int16_t s_last_level = -1;           // Force first report (-1 = uninitialized)
static uint8_t s_gpio_lower = 0;
static uint8_t s_gpio_upper = 0;
static bool s_active = false;
static uint32_t s_last_send_time = 0;       // Last time data was sent to queue

/* External queues */
extern QueueHandle_t g_sensor_data_queue;

// ============================================================================
// GPIO and Reading
// ============================================================================

/**
 * @brief Initialize GPIO pins for XKC sensors
 * Pull-down enabled, input mode, no interrupt (polled via timer)
 */
static void sensor_gpio_init(uint8_t gpio_lower, uint8_t gpio_upper)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << gpio_lower) | (1ULL << gpio_upper),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    ESP_LOGI(TAG, "GPIO initialized: lower=%d, upper=%d", gpio_lower, gpio_upper);
}

/**
 * @brief Read water level from two XKC sensors
 * @return 0 (empty), 1 (low), 2 (full)
 */
static int16_t read_water_level(void)
{
    int lower = gpio_get_level(s_gpio_lower);
    int upper = gpio_get_level(s_gpio_upper);
    return (int16_t)(lower + upper);
}

// ============================================================================
// Timer Callback
// ============================================================================

/**
 * @brief Periodic timer callback - reads GPIO and sends data to queue
 *
 * Sends data when:
 * 1. Value changed (and min_interval has elapsed since last send)
 * 2. Max interval keepalive (prevents false timeout in scheduler)
 */
static void xkc_timer_callback(void *arg)
{
    if (!s_active || g_sensor_data_queue == NULL) {
        return;
    }

    int16_t level = read_water_level();
    uint32_t now = (uint32_t)time(NULL);

    bool value_changed = (level != s_last_level);

    // Get reporting config from device manager for this virtual device
    uint16_t max_interval = 10;  // Default keepalive: 10 seconds
    uint16_t min_interval = 0;   // Default min: 0 (immediate)

    device_config_t dev;
    if (device_manager_get_by_type(LOCAL_XKC_IEEE_ADDR, DEVICE_TYPE_WATER_LEVEL_SENSOR, &dev) == ESP_OK) {
        if (dev.sensor.report_max_interval > 0) {
            max_interval = dev.sensor.report_max_interval;
        }
        min_interval = dev.sensor.report_min_interval;
    }

    bool keepalive_due = (now - s_last_send_time >= max_interval);
    bool min_interval_ok = (now - s_last_send_time >= min_interval);

    bool should_send = false;
    if (value_changed && min_interval_ok) {
        should_send = true;
    } else if (keepalive_due) {
        should_send = true;
    }

    if (should_send) {
        if (value_changed) {
            ESP_LOGI(TAG, "Water level changed: %d -> %d", s_last_level, level);
        }

        s_last_level = level;
        s_last_send_time = now;

        sensor_data_msg_t msg = {
            .ieee_addr = LOCAL_XKC_IEEE_ADDR,
            .endpoint = LOCAL_XKC_ENDPOINT,
            .cluster_id = 0x0403,  // Pressure measurement cluster (same as Zigbee XKC)
            .raw_value = level,
            .timestamp = now,
        };
        xQueueSend(g_sensor_data_queue, &msg, 0);  // Non-blocking
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t local_xkc_sensor_init(void)
{
    ESP_LOGI(TAG, "Local XKC sensor module initialized");
    return ESP_OK;
}

esp_err_t local_xkc_sensor_start(uint8_t gpio_lower, uint8_t gpio_upper)
{
    if (s_active) {
        ESP_LOGW(TAG, "Already active, stop first");
        return ESP_ERR_INVALID_STATE;
    }

    // Validate GPIOs
    if (!local_xkc_sensor_is_valid_gpio(gpio_lower) ||
        !local_xkc_sensor_is_valid_gpio(gpio_upper)) {
        ESP_LOGE(TAG, "Invalid GPIO pin(s): lower=%d, upper=%d", gpio_lower, gpio_upper);
        return ESP_ERR_INVALID_ARG;
    }
    if (gpio_lower == gpio_upper) {
        ESP_LOGE(TAG, "Lower and upper GPIO must be different");
        return ESP_ERR_INVALID_ARG;
    }

    // Save GPIO pins
    s_gpio_lower = gpio_lower;
    s_gpio_upper = gpio_upper;

    // Initialize GPIO
    sensor_gpio_init(gpio_lower, gpio_upper);

    // Add virtual device to device_manager if not already present
    int idx = device_manager_find_index_by_type(LOCAL_XKC_IEEE_ADDR, DEVICE_TYPE_WATER_LEVEL_SENSOR);
    if (idx < 0) {
        esp_err_t ret = device_manager_add_sensor(
            LOCAL_XKC_IEEE_ADDR,
            LOCAL_XKC_ENDPOINT,
            DEVICE_TYPE_WATER_LEVEL_SENSOR,
            "Local",        // manufacturer
            "XKC_GPIO"      // model
        );
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to add virtual device: %s", esp_err_to_name(ret));
            return ret;
        }

        // Set local sensor specific defaults (different from Zigbee defaults)
        device_config_t dev;
        if (device_manager_get_by_type(LOCAL_XKC_IEEE_ADDR, DEVICE_TYPE_WATER_LEVEL_SENSOR, &dev) == ESP_OK) {
            dev.sensor.report_min_interval = 0;    // Immediate on change
            dev.sensor.report_max_interval = 10;   // 10 second keepalive
            dev.sensor.report_change = 1;           // Any change (0→1, 1→2, etc.)
            device_manager_update_by_type(LOCAL_XKC_IEEE_ADDR, DEVICE_TYPE_WATER_LEVEL_SENSOR, &dev);
        }
    }

    // Reset state
    s_last_level = -1;  // Force first report
    s_last_send_time = 0;

    // Create and start periodic timer (1 second interval)
    esp_timer_create_args_t timer_args = {
        .callback = xkc_timer_callback,
        .name = "xkc_read",
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_read_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_timer_start_periodic(s_read_timer, 1000 * 1000);  // 1 second in microseconds
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_read_timer);
        s_read_timer = NULL;
        return ret;
    }

    s_active = true;
    ESP_LOGI(TAG, "Local XKC sensor started (GPIO lower=%d, upper=%d)", gpio_lower, gpio_upper);
    return ESP_OK;
}

esp_err_t local_xkc_sensor_stop(void)
{
    if (!s_active) {
        return ESP_OK;  // Already stopped, not an error
    }

    // Stop and delete timer
    if (s_read_timer != NULL) {
        esp_timer_stop(s_read_timer);
        esp_timer_delete(s_read_timer);
        s_read_timer = NULL;
    }

    // Reset GPIO pins to safe state
    gpio_reset_pin(s_gpio_lower);
    gpio_reset_pin(s_gpio_upper);

    // Remove virtual device from device_manager
    int idx = device_manager_find_index_by_type(LOCAL_XKC_IEEE_ADDR, DEVICE_TYPE_WATER_LEVEL_SENSOR);
    if (idx >= 0) {
        device_manager_remove(LOCAL_XKC_IEEE_ADDR);
    }

    s_active = false;
    s_last_level = -1;
    s_last_send_time = 0;

    ESP_LOGI(TAG, "Local XKC sensor stopped");
    return ESP_OK;
}

bool local_xkc_sensor_is_active(void)
{
    return s_active;
}

bool local_xkc_sensor_is_valid_gpio(uint8_t gpio_num)
{
    for (uint8_t i = 0; i < sizeof(s_valid_gpios); i++) {
        if (s_valid_gpios[i] == gpio_num) {
            return true;
        }
    }
    return false;
}

const uint8_t* local_xkc_sensor_get_valid_gpios(uint8_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_valid_gpios);
    }
    return s_valid_gpios;
}
