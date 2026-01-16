/**
 * @file button_task.c
 * @brief GPIO button handling task implementation
 */

#include "button_task.h"
#include "switch_driver.h"
#include "esp_log.h"

static const char *TAG = "BUTTON_TASK";

static button_callback_t s_button_callback = NULL;

static switch_func_pair_t s_button_func_pair[] = {
    {GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}
};

static void button_press_handler(switch_func_pair_t *param)
{
    if (param == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Button pressed on GPIO %lu, function %d",
             (unsigned long)param->pin, param->func);

    if (s_button_callback != NULL) {
        s_button_callback();
    }
}

esp_err_t button_task_init(button_callback_t callback)
{
    s_button_callback = callback;

    bool ret = switch_driver_init(
        s_button_func_pair,
        sizeof(s_button_func_pair) / sizeof(s_button_func_pair[0]),
        button_press_handler
    );

    if (!ret) {
        ESP_LOGE(TAG, "Failed to initialize switch driver");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Button task initialized on GPIO %d", GPIO_INPUT_IO_TOGGLE_SWITCH);
    return ESP_OK;
}
