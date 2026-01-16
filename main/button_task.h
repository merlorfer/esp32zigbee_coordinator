/**
 * @file button_task.h
 * @brief GPIO button handling task
 */

#pragma once

#include "common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button press callback type
 */
typedef void (*button_callback_t)(void);

/**
 * @brief Initialize button task
 * @param callback Function to call when button is pressed
 * @return ESP_OK on success
 */
esp_err_t button_task_init(button_callback_t callback);

#ifdef __cplusplus
}
#endif
