/**
 * @file scheduler_task.h
 * @brief Automation scheduler task
 */

#pragma once

#include "common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize scheduler task
 * @return ESP_OK on success
 */
esp_err_t scheduler_task_init(void);

/**
 * @brief Start scheduler task
 * @return ESP_OK on success
 */
esp_err_t scheduler_task_start(void);

/**
 * @brief Stop scheduler task (when Wi-Fi AP activates)
 */
void scheduler_task_stop(void);

/**
 * @brief Resume scheduler task (when Wi-Fi AP deactivates)
 */
void scheduler_task_resume(void);

/**
 * @brief Reset delay cycle start time for all devices
 */
void scheduler_reset_delay_cycles(void);

#ifdef __cplusplus
}
#endif
