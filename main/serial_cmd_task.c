#include "serial_cmd_task.h"
#include "ble_handlers.h"
#include "device_manager.h"
#include "log_manager.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define TAG "SERIAL_CMD"
#define LINE_BUF_SIZE 2048
#define RX_BUF_SIZE   1024

#define SERIAL_UART_NUM      UART_NUM_0
#define SERIAL_UART_BAUD     115200
#define SERIAL_UART_TX_PIN   21      // U0TXD — no conflict with GPIO9 (btn) or GPIO15 (led)
#define SERIAL_UART_RX_PIN   20      // U0RXD

static bool s_use_uart = false;

// ---------------------------------------------------------------------------
// Thin wrappers to unify read/write across the two drivers.
// uart_write_bytes has no timeout param so the ticks argument is ignored.
// ---------------------------------------------------------------------------

static int serial_read(uint8_t *buf, int len, TickType_t ticks)
{
    if (s_use_uart) {
        return uart_read_bytes(SERIAL_UART_NUM, buf, (uint32_t)len, ticks);
    }
    return usb_serial_jtag_read_bytes(buf, len, ticks);
}

static int serial_write(const uint8_t *buf, int len, TickType_t ticks)
{
    if (s_use_uart) {
        return uart_write_bytes(SERIAL_UART_NUM, (const char *)buf, (size_t)len);
    }
    return usb_serial_jtag_write_bytes(buf, len, ticks);
}

// ---------------------------------------------------------------------------
// Command task — identical protocol regardless of driver
// ---------------------------------------------------------------------------

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
        int n = serial_read(rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)rx_buf[i];
            if (c == '\n' || c == '\r') {
                if (line_len == 0) continue;
                line_buf[line_len] = '\0';

                if (line_buf[0] == '{') {
                    char *response = ble_handlers_process_command(line_buf, line_len);
                    if (response) {
                        int resp_len = strlen(response);
                        // Write in chunks — USB-JTAG TX ring buffer is 4096 bytes,
                        // UART TX ring buffer is 4096 bytes (see uart_driver_install below).
                        #define TX_CHUNK 2048
                        serial_write((const uint8_t *)">>>", 3, pdMS_TO_TICKS(200));
                        int sent = 0;
                        while (sent < resp_len) {
                            int chunk = resp_len - sent;
                            if (chunk > TX_CHUNK) chunk = TX_CHUNK;
                            serial_write((const uint8_t *)(response + sent), chunk, pdMS_TO_TICKS(500));
                            sent += chunk;
                        }
                        serial_write((const uint8_t *)"\n", 1, pdMS_TO_TICKS(200));
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

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t serial_cmd_task_init(void)
{
    global_config_t gconfig;
    device_manager_get_global_config(&gconfig);
    s_use_uart = (gconfig.serial_interface == 1);

    if (s_use_uart) {
        uart_config_t uart_cfg = {
            .baud_rate  = SERIAL_UART_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        // RX buf 2×RX_BUF_SIZE to absorb bursts; TX buf 4096 matches USB-JTAG default.
        esp_err_t ret = uart_driver_install(SERIAL_UART_NUM,
                                            RX_BUF_SIZE * 2, 4096,
                                            0, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = uart_param_config(SERIAL_UART_NUM, &uart_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = uart_set_pin(SERIAL_UART_NUM,
                           SERIAL_UART_TX_PIN, SERIAL_UART_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
            return ret;
        }

        // Driver is up — redirect log output to UART0 from this point on.
        log_manager_set_serial_interface(1);

        ESP_LOGI(TAG, "Serial interface: UART0  TX=GPIO%d  RX=GPIO%d  %d baud",
                 SERIAL_UART_TX_PIN, SERIAL_UART_RX_PIN, SERIAL_UART_BAUD);
    } else {
        usb_serial_jtag_driver_config_t cfg = {
            .rx_buffer_size = RX_BUF_SIZE,
            .tx_buffer_size = 4096,
        };
        esp_err_t ret = usb_serial_jtag_driver_install(&cfg);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Serial interface: USB Serial JTAG");
    }

    BaseType_t res = xTaskCreate(serial_cmd_task, "serial_cmd", 4096, NULL, 3, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial_cmd task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Serial command interface ready (response prefix: >>>)");
    return ESP_OK;
}
