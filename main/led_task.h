/**
 * @file led_task.h
 * @brief LED status indication task
 */

#pragma once

#include "common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize LED task
 * @return ESP_OK on success
 */
esp_err_t led_task_init(void);

/**
 * @brief Set LED state
 * @param state New LED state
 */
void led_set_state(led_state_t state);

/**
 * @brief Get current LED state
 * @return Current LED state
 */
led_state_t led_get_state(void);

#ifdef __cplusplus
}
#endif
