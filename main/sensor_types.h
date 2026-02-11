/**
 * @file sensor_types.h
 * @brief Central sensor type registry - all sensor type info in one place
 *
 * To add a new sensor type:
 * 1. Add enum value to device_type_t in common.h
 * 2. Add entry to s_sensor_types[] in sensor_types.c
 */

#pragma once

#include "common.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    device_type_t device_type;
    uint16_t cluster_id;           // ZCL cluster (0x0402, 0x0405, ...)
    const char *type_string;       // "temperature_sensor", "humidity_sensor"
    const char *display_name;      // "Temperature", "Humidity"
    const char *unit;              // "°C", "%"
    uint8_t zcl_attr_type;         // ESP_ZB_ZCL_ATTR_TYPE_S16 or U16
    float default_lower_threshold;
    float default_upper_threshold;
    float default_hysteresis;
    float conversion_divisor;      // 100.0
    bool is_signed;                // true = INT16, false = UINT16
} sensor_type_info_t;

/**
 * @brief Get sensor type info by device_type enum
 * @return pointer to info or NULL if not a sensor type
 */
const sensor_type_info_t* sensor_type_get_info(device_type_t type);

/**
 * @brief Check if a device_type is a sensor
 */
bool is_sensor_device(device_type_t type);

/**
 * @brief Get sensor type info by ZCL cluster ID
 * @return pointer to info or NULL if unknown cluster
 */
const sensor_type_info_t* sensor_type_get_by_cluster(uint16_t cluster_id);

/**
 * @brief Get sensor type info by type string (e.g. "temperature_sensor")
 * @return pointer to info or NULL if unknown string
 */
const sensor_type_info_t* sensor_type_get_by_string(const char *type_str);

/**
 * @brief Convert device_type to string. Returns "on_off_light" for non-sensor types.
 */
const char* sensor_type_to_string(device_type_t type);

/**
 * @brief Convert type string to device_type enum. Returns DEVICE_TYPE_ON_OFF_LIGHT for unknown.
 */
device_type_t sensor_type_from_string(const char *type_str);

/**
 * @brief Get number of registered sensor types
 */
uint8_t sensor_type_get_count(void);

#ifdef __cplusplus
}
#endif
