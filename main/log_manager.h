/**
 * @file log_manager.h
 * @brief Log manager - FAT-based persistent session logging with live RAM ring buffer
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#define LOG_RAM_BUF 8192

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize log manager: mount FAT, create session file, install vprintf hook.
 *        Must be called before any other log_manager function.
 */
esp_err_t log_manager_init(void);

/**
 * @brief Flush pending buffer to FAT session file.
 *        Safe to call from any task.
 */
void log_manager_flush(void);

/**
 * @brief Copy live RAM ring buffer contents as null-terminated string.
 * @param out     Output buffer
 * @param out_size Size of output buffer
 */
void log_manager_get_live(char *out, size_t out_size);

/**
 * @brief Populate a cJSON array with session file info objects.
 *        Each object: {"name": "session_N.log", "size": N, "current": bool}
 */
esp_err_t log_manager_list_sessions(cJSON *arr);

/**
 * @brief Send a session file as chunked HTTP response (plain/text).
 * @param name  Filename, e.g. "session_0.log"
 * @param req   HTTP request handle
 */
esp_err_t log_manager_get_session(const char *name, httpd_req_t *req);

/**
 * @brief Delete all session files and reset session index.
 */
esp_err_t log_manager_clear(void);

/**
 * @brief Set log filter mode.
 *        If zigbee_only=true: BLE/WiFi log tags set to ESP_LOG_NONE.
 *        If zigbee_only=false: those tags restored to ESP_LOG_INFO.
 */
void log_manager_set_filter(bool zigbee_only);

#ifdef __cplusplus
}
#endif
