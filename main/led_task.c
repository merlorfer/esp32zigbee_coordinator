/**
 * @file led_task.c
 * @brief LED status indication task implementation
 */

#include "led_task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LED_TASK";

static led_state_t s_current_state = LED_STATE_NORMAL;
static TaskHandle_t s_led_task_handle = NULL;

static void led_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_LED_STATUS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(GPIO_LED_STATUS, 0);
}

static void led_set_level(bool on)
{
    gpio_set_level(GPIO_LED_STATUS, on ? 1 : 0);
}

static void led_task(void *pvParameters)
{
    led_state_t current_state;
    int blink_counter = 0;

    ESP_LOGI(TAG, "LED task started");

    for (;;) {
        current_state = s_current_state;

        switch (current_state) {
            case LED_STATE_NORMAL:
                // 1 second blink (500ms ON, 500ms OFF)
                led_set_level(blink_counter % 2 == 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                blink_counter++;
                break;

            case LED_STATE_WIFI_ACTIVE:
                // Solid ON
                led_set_level(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case LED_STATE_PAIRING:
                // Fast blink for pairing mode (125ms ON, 125ms OFF = 0.25s cycle)
                led_set_level(blink_counter % 2 == 0);
                vTaskDelay(pdMS_TO_TICKS(125));
                blink_counter++;
                break;

            case LED_STATE_ERROR:
                // 3x fast blink, then pause
                // Pattern: ON-OFF-ON-OFF-ON-OFF-PAUSE
                if (blink_counter < 6) {
                    led_set_level(blink_counter % 2 == 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                } else {
                    led_set_level(false);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                blink_counter++;
                if (blink_counter >= 7) {
                    blink_counter = 0;
                }
                break;

            default:
                led_set_level(false);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}

esp_err_t led_task_init(void)
{
    led_gpio_init();

    BaseType_t ret = xTaskCreate(
        led_task,
        "led_task",
        TASK_STACK_LED,
        NULL,
        TASK_PRIORITY_LED,
        &s_led_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LED task initialized");
    return ESP_OK;
}

void led_set_state(led_state_t state)
{
    s_current_state = state;
    ESP_LOGI(TAG, "LED state changed to %d", state);
}

led_state_t led_get_state(void)
{
    return s_current_state;
}
