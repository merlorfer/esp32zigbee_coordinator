/**
 * @file local_xkc_sensor.h
 * @brief Local XKC water level sensor (GPIO-direct, no Zigbee)
 *
 * Supports 2 XKC non-contact water level sensors connected directly
 * to the coordinator ESP32-C6 GPIO pins. Creates a virtual device
 * in the device manager that behaves like a Zigbee-paired sensor.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Safe GPIO choices for ESP32-C6 in digital mode
 * Excluded: 0 (strapping), 1-2 (UART/USB), 8 (SPI), 9 (button),
 *           12-13 (USB-C D+/D-), 15 (LED)
 */
#define VALID_XKC_GPIO_COUNT  14

/* Analog mode is limited to ADC1-capable pins -- on the ESP32-C6, ADC1 only
 * covers GPIO0-GPIO6, so the intersection with the digital-mode safe list
 * above is just GPIO 3, 4, 5, 6. */
#define VALID_XKC_ANALOG_GPIO_COUNT  4

/**
 * @brief Initialize local XKC sensor module (does not start reading)
 */
esp_err_t local_xkc_sensor_init(void);

/**
 * @brief Start reading from local XKC sensor
 * @param gpio_lower GPIO pin for lower sensor
 * @param gpio_upper GPIO pin for upper sensor
 * @param sense_mode XKC_SENSE_MODE_DIGITAL or XKC_SENSE_MODE_ANALOG (common.h)
 * @param threshold_mv Analog mode only: mV at/above which a channel reads as "high"
 * @return ESP_OK on success
 *
 * Adds a virtual DEVICE_TYPE_WATER_LEVEL_SENSOR to the device manager.
 * Starts a periodic timer that reads the two inputs (as plain GPIO levels
 * in digital mode, or as ADC voltages compared against threshold_mv in
 * analog mode) and pushes data to g_sensor_data_queue on value change or
 * keepalive interval.
 */
esp_err_t local_xkc_sensor_start(uint8_t gpio_lower, uint8_t gpio_upper,
                                  uint8_t sense_mode, uint16_t threshold_mv);

/**
 * @brief Stop reading, releasing the GPIO/ADC resources for the active mode
 * @param remove_device If true, also deletes the virtual device from the
 *        device manager (its custom name, thresholds, links, etc. are
 *        lost) -- use this for a genuine disable/delete. Pass false when
 *        stopping only to immediately restart with a new GPIO/sense mode,
 *        so the existing device entry (and whatever the user has
 *        configured on it) survives the reconfiguration.
 */
esp_err_t local_xkc_sensor_stop(bool remove_device);

/**
 * @brief Check if local XKC sensor is currently active
 */
bool local_xkc_sensor_is_active(void);

/**
 * @brief Validate that a GPIO pin is in the safe list for the given sense mode
 */
bool local_xkc_sensor_is_valid_gpio(uint8_t gpio_num, uint8_t sense_mode);

/**
 * @brief Get array of valid GPIO pins and count for the given sense mode
 * @param sense_mode XKC_SENSE_MODE_DIGITAL or XKC_SENSE_MODE_ANALOG (common.h)
 * @param count Pointer to store number of valid pins
 * @return Pointer to static array of valid GPIO pin numbers
 */
const uint8_t* local_xkc_sensor_get_valid_gpios(uint8_t sense_mode, uint8_t *count);

#ifdef __cplusplus
}
#endif
