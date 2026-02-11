/**
 * @file sensor_types.c
 * @brief Central sensor type registry implementation
 */

#include "sensor_types.h"
#include "esp_zigbee_core.h"
#include <string.h>

static const sensor_type_info_t s_sensor_types[] = {
    {
        .device_type = DEVICE_TYPE_TEMPERATURE_SENSOR,
        .cluster_id = 0x0402,  // ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT
        .type_string = "temperature_sensor",
        .display_name = "Temperature",
        .unit = "\xC2\xB0""C",  // °C in UTF-8
        .zcl_attr_type = ESP_ZB_ZCL_ATTR_TYPE_S16,
        .default_lower_threshold = 18.0f,
        .default_upper_threshold = 25.0f,
        .default_hysteresis = 0.5f,
        .conversion_divisor = 100.0f,
        .is_signed = true,
    },
    {
        .device_type = DEVICE_TYPE_HUMIDITY_SENSOR,
        .cluster_id = 0x0405,  // ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT
        .type_string = "humidity_sensor",
        .display_name = "Humidity",
        .unit = "%",
        .zcl_attr_type = ESP_ZB_ZCL_ATTR_TYPE_U16,
        .default_lower_threshold = 30.0f,
        .default_upper_threshold = 70.0f,
        .default_hysteresis = 2.0f,
        .conversion_divisor = 100.0f,
        .is_signed = false,
    },
};

#define SENSOR_TYPE_COUNT (sizeof(s_sensor_types) / sizeof(s_sensor_types[0]))

const sensor_type_info_t* sensor_type_get_info(device_type_t type)
{
    for (uint8_t i = 0; i < SENSOR_TYPE_COUNT; i++) {
        if (s_sensor_types[i].device_type == type) {
            return &s_sensor_types[i];
        }
    }
    return NULL;
}

bool is_sensor_device(device_type_t type)
{
    return sensor_type_get_info(type) != NULL;
}

const sensor_type_info_t* sensor_type_get_by_cluster(uint16_t cluster_id)
{
    for (uint8_t i = 0; i < SENSOR_TYPE_COUNT; i++) {
        if (s_sensor_types[i].cluster_id == cluster_id) {
            return &s_sensor_types[i];
        }
    }
    return NULL;
}

const sensor_type_info_t* sensor_type_get_by_string(const char *type_str)
{
    if (type_str == NULL) return NULL;
    for (uint8_t i = 0; i < SENSOR_TYPE_COUNT; i++) {
        if (strcmp(s_sensor_types[i].type_string, type_str) == 0) {
            return &s_sensor_types[i];
        }
    }
    return NULL;
}

const char* sensor_type_to_string(device_type_t type)
{
    const sensor_type_info_t *info = sensor_type_get_info(type);
    if (info != NULL) {
        return info->type_string;
    }
    return "on_off_light";
}

device_type_t sensor_type_from_string(const char *type_str)
{
    const sensor_type_info_t *info = sensor_type_get_by_string(type_str);
    if (info != NULL) {
        return info->device_type;
    }
    return DEVICE_TYPE_ON_OFF_LIGHT;
}

uint8_t sensor_type_get_count(void)
{
    return SENSOR_TYPE_COUNT;
}
