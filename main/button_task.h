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
 * @brief Long press callback type
 */
typedef void (*button_long_press_callback_t)(void);

/**
 * @brief Initialize button task
 * @param short_press_callback Function to call on short button press (< 3 sec)
 * @param long_press_callback Function to call on long button press (>= 3 sec)
 * @return ESP_OK on success
 */
esp_err_t button_task_init(button_callback_t short_press_callback,
                           button_long_press_callback_t long_press_callback);

#ifdef __cplusplus
}
#endif
