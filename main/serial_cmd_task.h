#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the serial command task.
 *
 * Installs the USB Serial JTAG driver for RX and creates the task that
 * listens for newline-terminated JSON commands on the USB serial port.
 * Responses are written back with a ">>>" prefix so the host can
 * distinguish them from ESP_LOG output lines.
 *
 * @return ESP_OK on success
 */
esp_err_t serial_cmd_task_init(void);
