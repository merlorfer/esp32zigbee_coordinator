/**
 * @file local_xkc_sensor.c
 * @brief Local XKC water level sensor implementation
 *
 * Uses esp_timer (no new FreeRTOS task) to periodically read two XKC
 * non-contact water level sensors, either as plain GPIO levels (digital
 * mode) or as ADC voltages compared against a software threshold (analog
 * mode -- an interim workaround for sensors whose "high" output doesn't
 * reach a reliable digital logic level at 3.3V, until a proper level
 * shifter is installed). Data is fed through the existing
 * g_sensor_data_queue so the scheduler handles thresholds, timeouts, and
 * linked device automation automatically either way.
 */

#include "local_xkc_sensor.h"
#include "common.h"
#include "device_manager.h"
#include "sensor_types.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <string.h>
#include <time.h>

static const char *TAG = "LOCAL_XKC";

/* Valid GPIO pins for XKC sensors on ESP32-C6, digital mode */
static const uint8_t s_valid_gpios_digital[] = {
    3, 4, 5, 6, 7, 10, 11, 14, 18, 19, 20, 21, 22, 23
};

/* Analog mode: ESP32-C6 ADC1 only covers GPIO0-GPIO6, intersected with the
 * digital-mode safe list above. */
static const uint8_t s_valid_gpios_analog[] = {
    3, 4, 5, 6
};

/* Module state */
static esp_timer_handle_t s_read_timer = NULL;
static int16_t s_last_level = -1;           // Force first report (-1 = uninitialized)
static uint8_t s_gpio_lower = 0;
static uint8_t s_gpio_upper = 0;
static uint8_t s_sense_mode = XKC_SENSE_MODE_DIGITAL;
static uint16_t s_threshold_mv = DEFAULT_XKC_THRESHOLD_MV;
static bool s_active = false;
static uint32_t s_last_send_time = 0;       // Last time data was sent to queue

/* Analog mode ADC state */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_lower = NULL;
static adc_cali_handle_t s_adc_cali_upper = NULL;
static adc_channel_t s_adc_ch_lower = 0;
static adc_channel_t s_adc_ch_upper = 0;

/* External queues */
extern QueueHandle_t g_sensor_data_queue;

// ============================================================================
// GPIO / ADC init and reading
// ============================================================================

static void analog_init(uint8_t gpio_lower, uint8_t gpio_upper)
{
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&init_cfg, &s_adc_handle);

    adc_unit_t unit;
    adc_oneshot_io_to_channel(gpio_lower, &unit, &s_adc_ch_lower);
    adc_oneshot_io_to_channel(gpio_upper, &unit, &s_adc_ch_upper);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,      // widest range, needed since the
                                       // unshifted sensor signal can exceed 2V
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(s_adc_handle, s_adc_ch_lower, &chan_cfg);
    adc_oneshot_config_channel(s_adc_handle, s_adc_ch_upper, &chan_cfg);

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    cali_cfg.chan = s_adc_ch_lower;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_lower) != ESP_OK) {
        s_adc_cali_lower = NULL;
        ESP_LOGW(TAG, "ADC calibration unavailable for lower channel; using raw counts");
    }
    cali_cfg.chan = s_adc_ch_upper;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_upper) != ESP_OK) {
        s_adc_cali_upper = NULL;
        ESP_LOGW(TAG, "ADC calibration unavailable for upper channel; using raw counts");
    }

    ESP_LOGI(TAG, "ADC initialized: lower=GPIO%d upper=GPIO%d threshold=%dmV",
             gpio_lower, gpio_upper, s_threshold_mv);
}

static void analog_deinit(void)
{
    if (s_adc_cali_lower) {
        adc_cali_delete_scheme_curve_fitting(s_adc_cali_lower);
        s_adc_cali_lower = NULL;
    }
    if (s_adc_cali_upper) {
        adc_cali_delete_scheme_curve_fitting(s_adc_cali_upper);
        s_adc_cali_upper = NULL;
    }
    if (s_adc_handle) {
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
    }
}

/**
 * @brief Initialize the two sensor inputs for the active sense mode
 */
static void sensor_io_init(uint8_t gpio_lower, uint8_t gpio_upper, uint8_t sense_mode)
{
    if (sense_mode == XKC_SENSE_MODE_ANALOG) {
        analog_init(gpio_lower, gpio_upper);
    } else {
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
}

static int read_channel_mv(adc_channel_t chan, adc_cali_handle_t cali)
{
    int raw = 0;
    adc_oneshot_read(s_adc_handle, chan, &raw);
    if (cali != NULL) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
            return mv;
        }
    }
    // Fallback without calibration: rough linear estimate for ADC_ATTEN_DB_12
    // (~0-3900mV over the 12-bit range) -- good enough for the "far from
    // threshold either way" comparisons this module needs.
    return (raw * 3900) / 4095;
}

/**
 * @brief Read water level from the two sensor inputs
 * @return 0 (empty), 1 (low), 2 (full)
 */
static int16_t read_water_level(void)
{
    int lower, upper;

    if (s_sense_mode == XKC_SENSE_MODE_ANALOG) {
        int mv_lower = read_channel_mv(s_adc_ch_lower, s_adc_cali_lower);
        int mv_upper = read_channel_mv(s_adc_ch_upper, s_adc_cali_upper);
        lower = (mv_lower >= s_threshold_mv) ? 1 : 0;
        upper = (mv_upper >= s_threshold_mv) ? 1 : 0;
    } else {
        lower = gpio_get_level(s_gpio_lower);
        upper = gpio_get_level(s_gpio_upper);
    }

    return (int16_t)(lower + upper);
}

// ============================================================================
// Timer Callback
// ============================================================================

/**
 * @brief Periodic timer callback - reads the sensor inputs and sends data to queue
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
        sensor_data_msg_t msg = {
            .ieee_addr = LOCAL_XKC_IEEE_ADDR,
            .endpoint = LOCAL_XKC_ENDPOINT,
            .cluster_id = 0x0403,  // Pressure measurement cluster (same as Zigbee XKC)
            .raw_value = level,
            .timestamp = now,
        };

        // Only advance our own state once the message actually made it into
        // the queue. If the queue is full (e.g. the scheduler is stalled
        // waiting on the RTC), leave s_last_level/s_last_send_time alone so
        // this same reading is retried on the next tick instead of being
        // silently considered "sent" and lost for good.
        if (xQueueSend(g_sensor_data_queue, &msg, 0) == pdTRUE) {
            if (value_changed) {
                ESP_LOGI(TAG, "Water level changed: %d -> %d", s_last_level, level);
            }
            s_last_level = level;
            s_last_send_time = now;
        } else {
            ESP_LOGW(TAG, "Failed to queue sensor data (queue full) - will retry next tick");
        }
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

esp_err_t local_xkc_sensor_start(uint8_t gpio_lower, uint8_t gpio_upper,
                                  uint8_t sense_mode, uint16_t threshold_mv)
{
    if (s_active) {
        ESP_LOGW(TAG, "Already active, stop first");
        return ESP_ERR_INVALID_STATE;
    }

    // Validate GPIOs against the safe list for the requested sense mode
    if (!local_xkc_sensor_is_valid_gpio(gpio_lower, sense_mode) ||
        !local_xkc_sensor_is_valid_gpio(gpio_upper, sense_mode)) {
        ESP_LOGE(TAG, "Invalid GPIO pin(s) for mode %d: lower=%d, upper=%d", sense_mode, gpio_lower, gpio_upper);
        return ESP_ERR_INVALID_ARG;
    }
    if (gpio_lower == gpio_upper) {
        ESP_LOGE(TAG, "Lower and upper GPIO must be different");
        return ESP_ERR_INVALID_ARG;
    }

    // Save state
    s_gpio_lower = gpio_lower;
    s_gpio_upper = gpio_upper;
    s_sense_mode = sense_mode;
    s_threshold_mv = (threshold_mv > 0) ? threshold_mv : DEFAULT_XKC_THRESHOLD_MV;

    // Initialize inputs for the active mode
    sensor_io_init(gpio_lower, gpio_upper, sense_mode);

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
    ESP_LOGI(TAG, "Local XKC sensor started (%s, GPIO lower=%d, upper=%d)",
             sense_mode == XKC_SENSE_MODE_ANALOG ? "analog" : "digital", gpio_lower, gpio_upper);
    return ESP_OK;
}

esp_err_t local_xkc_sensor_stop(bool remove_device)
{
    // Only remove the virtual device (losing its custom name, thresholds,
    // links, etc.) when this is a genuine disable/delete -- not when we're
    // just about to restart with a different GPIO/sense mode, in which case
    // local_xkc_sensor_start() will find the existing entry and reuse it.
    if (remove_device) {
        int idx = device_manager_find_index_by_type(LOCAL_XKC_IEEE_ADDR, DEVICE_TYPE_WATER_LEVEL_SENSOR);
        if (idx >= 0) {
            device_manager_remove(LOCAL_XKC_IEEE_ADDR);
        }
    }

    if (!s_active) {
        return ESP_OK;  // No timer/GPIO/ADC resources to clean up
    }

    // Stop and delete timer
    if (s_read_timer != NULL) {
        esp_timer_stop(s_read_timer);
        esp_timer_delete(s_read_timer);
        s_read_timer = NULL;
    }

    // Release whichever resources the active mode was using
    if (s_sense_mode == XKC_SENSE_MODE_ANALOG) {
        analog_deinit();
    } else {
        gpio_reset_pin(s_gpio_lower);
        gpio_reset_pin(s_gpio_upper);
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

bool local_xkc_sensor_is_valid_gpio(uint8_t gpio_num, uint8_t sense_mode)
{
    uint8_t count;
    const uint8_t *list = local_xkc_sensor_get_valid_gpios(sense_mode, &count);
    for (uint8_t i = 0; i < count; i++) {
        if (list[i] == gpio_num) {
            return true;
        }
    }
    return false;
}

const uint8_t* local_xkc_sensor_get_valid_gpios(uint8_t sense_mode, uint8_t *count)
{
    if (sense_mode == XKC_SENSE_MODE_ANALOG) {
        if (count != NULL) {
            *count = sizeof(s_valid_gpios_analog);
        }
        return s_valid_gpios_analog;
    }

    if (count != NULL) {
        *count = sizeof(s_valid_gpios_digital);
    }
    return s_valid_gpios_digital;
}
