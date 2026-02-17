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

/* Safe GPIO choices for ESP32-C6
 * Excluded: 0 (strapping), 1-2 (UART/USB), 8 (SPI), 9 (button),
 *           12-13 (USB-C D+/D-), 15 (LED)
 */
#define VALID_XKC_GPIO_COUNT  14

/**
 * @brief Initialize local XKC sensor module (does not start reading)
 */
esp_err_t local_xkc_sensor_init(void);

/**
 * @brief Start reading from local XKC sensor
 * @param gpio_lower GPIO pin for lower sensor
 * @param gpio_upper GPIO pin for upper sensor
 * @return ESP_OK on success
 *
 * Adds a virtual DEVICE_TYPE_WATER_LEVEL_SENSOR to the device manager.
 * Starts a periodic timer that reads GPIOs and pushes data
 * to g_sensor_data_queue on value change or keepalive interval.
 */
esp_err_t local_xkc_sensor_start(uint8_t gpio_lower, uint8_t gpio_upper);

/**
 * @brief Stop reading and remove virtual device
 */
esp_err_t local_xkc_sensor_stop(void);

/**
 * @brief Check if local XKC sensor is currently active
 */
bool local_xkc_sensor_is_active(void);

/**
 * @brief Validate that a GPIO pin is in the safe list
 */
bool local_xkc_sensor_is_valid_gpio(uint8_t gpio_num);

/**
 * @brief Get array of valid GPIO pins and count
 * @param count Pointer to store number of valid pins
 * @return Pointer to static array of valid GPIO pin numbers
 */
const uint8_t* local_xkc_sensor_get_valid_gpios(uint8_t *count);

#ifdef __cplusplus
}
#endif
