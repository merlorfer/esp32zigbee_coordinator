#include "serial_cmd_task.h"
#include "ble_handlers.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define TAG "SERIAL_CMD"
#define LINE_BUF_SIZE 2048
#define RX_BUF_SIZE   1024

static void serial_cmd_task(void *arg)
{
    char *line_buf = malloc(LINE_BUF_SIZE);
    if (!line_buf) {
        ESP_LOGE(TAG, "Failed to allocate line buffer");
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx_buf[128];
    int line_len = 0;

    for (;;) {
        int n = usb_serial_jtag_read_bytes(rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)rx_buf[i];
            if (c == '\n' || c == '\r') {
                if (line_len == 0) continue;
                line_buf[line_len] = '\0';

                if (line_buf[0] == '{') {
                    char *response = ble_handlers_process_command(line_buf, line_len);
                    if (response) {
                        printf(">>>%s\n", response);
                        fflush(stdout);
                        free(response);
                    }
                }
                line_len = 0;
            } else {
                if (line_len < LINE_BUF_SIZE - 1) {
                    line_buf[line_len++] = c;
                }
            }
        }
    }
}

esp_err_t serial_cmd_task_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = RX_BUF_SIZE,
        .tx_buffer_size = 256,
    };
    esp_err_t ret = usb_serial_jtag_driver_install(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t res = xTaskCreate(serial_cmd_task, "serial_cmd", 4096, NULL, 3, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial_cmd task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Serial command interface ready (prefix: >>>)");
    return ESP_OK;
}
