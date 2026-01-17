/**
 * @file button_task.c
 * @brief GPIO button handling task implementation
 */

#include "button_task.h"
#include "switch_driver.h"
#include "esp_log.h"

static const char *TAG = "BUTTON_TASK";

static button_callback_t s_short_press_callback = NULL;
static button_long_press_callback_t s_long_press_callback = NULL;

static switch_func_pair_t s_button_func_pair[] = {
    {GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}
};

static void button_short_press_handler(switch_func_pair_t *param)
{
    if (param == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Short press on GPIO %lu", (unsigned long)param->pin);

    if (s_short_press_callback != NULL) {
        s_short_press_callback();
    }
}

static void button_long_press_handler(switch_func_pair_t *param)
{
    if (param == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Long press on GPIO %lu", (unsigned long)param->pin);

    if (s_long_press_callback != NULL) {
        s_long_press_callback();
    }
}

esp_err_t button_task_init(button_callback_t short_press_callback,
                           button_long_press_callback_t long_press_callback)
{
    s_short_press_callback = short_press_callback;
    s_long_press_callback = long_press_callback;

    bool ret = switch_driver_init(
        s_button_func_pair,
        sizeof(s_button_func_pair) / sizeof(s_button_func_pair[0]),
        button_short_press_handler
    );

    if (!ret) {
        ESP_LOGE(TAG, "Failed to initialize switch driver");
        return ESP_FAIL;
    }

    // Set the long press callback
    switch_driver_set_long_press_callback(button_long_press_handler);

    ESP_LOGI(TAG, "Button task initialized on GPIO %d", GPIO_INPUT_IO_TOGGLE_SWITCH);
    ESP_LOGI(TAG, "  Short press (<3s): Mode toggle");
    ESP_LOGI(TAG, "  Long press (>=3s): Zigbee pairing");
    return ESP_OK;
}
