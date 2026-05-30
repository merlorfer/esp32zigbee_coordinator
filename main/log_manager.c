/**
 * @file log_manager.c
 * @brief FAT-based persistent session logging with live RAM ring buffer
 */

#include "log_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>   // fsync()

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "driver/uart.h"

static const char *TAG = "log_manager";

// ============================================================================
// Constants
// ============================================================================

#define LOG_MOUNT_POINT     "/logs"
#define LOG_PARTITION_LABEL "log_store"
#define LOG_MAX_SESSIONS    3
#define LOG_IDX_FILE        "/logs/idx"

// FAT 8.3 filename constraint: base name ≤ 8 chars, ext ≤ 3 chars
// "sess0.log" = 5+1 chars before dot → valid; "session_0.log" = 9 chars → invalid with LFN disabled

#define LOG_FLUSH_BUF       2048

// ============================================================================
// State
// ============================================================================

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static FILE       *s_session_file = NULL;
static int         s_current_session = 0;
static bool        s_initialized = false;

// RAM ring buffer (live log view)
static char              s_ram_buf[LOG_RAM_BUF];
static int               s_ram_head = 0;
static int               s_ram_tail = 0;
static int               s_ram_len  = 0;
static portMUX_TYPE      s_ram_mux  = portMUX_INITIALIZER_UNLOCKED;

// Flush buffer (FAT writes)
static char              s_flush_buf[LOG_FLUSH_BUF];
static int               s_flush_pos = 0;
static portMUX_TYPE      s_flush_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool     s_flush_needed = false;

// (no timer handle needed — flush runs in its own task)
static SemaphoreHandle_t s_flush_sem   = NULL;  // serializes flush calls
static char              s_flush_local[LOG_FLUSH_BUF];  // avoids stack alloc in timer cb

// Serial interface selection: 0 = default stdout/USB-JTAG, 1 = UART0
// Set to 1 only after uart_driver_install() has succeeded (called by serial_cmd_task).
static volatile uint8_t  s_serial_iface = 0;

// ============================================================================
// Internal helpers
// ============================================================================

static void write_to_ram_buf(const char *data, int len)
{
    if (len <= 0) return;
    // If data is larger than the whole buffer, only keep the tail part
    if (len >= LOG_RAM_BUF) {
        data += (len - LOG_RAM_BUF + 1);
        len   = LOG_RAM_BUF - 1;
    }

    portENTER_CRITICAL(&s_ram_mux);
    for (int i = 0; i < len; i++) {
        s_ram_buf[s_ram_tail] = data[i];
        s_ram_tail = (s_ram_tail + 1) % LOG_RAM_BUF;
        if (s_ram_len < LOG_RAM_BUF) {
            s_ram_len++;
        } else {
            // Overwrite: advance head
            s_ram_head = (s_ram_head + 1) % LOG_RAM_BUF;
        }
    }
    portEXIT_CRITICAL(&s_ram_mux);
}

static void append_to_flush_buf(const char *data, int len)
{
    if (len <= 0) return;
    portENTER_CRITICAL(&s_flush_mux);
    int avail = LOG_FLUSH_BUF - s_flush_pos;
    int copy  = (len < avail) ? len : avail;
    if (copy > 0) {
        memcpy(s_flush_buf + s_flush_pos, data, copy);
        s_flush_pos += copy;
    }
    portEXIT_CRITICAL(&s_flush_mux);
}

static int log_vprintf_hook(const char *fmt, va_list args)
{
    char line[256];
    int len = vsnprintf(line, sizeof(line), fmt, args);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(line)) len = sizeof(line) - 1;

    // 1. Console output — UART0 when interface switched, default stdout otherwise
    if (s_serial_iface == 1) {
        uart_write_bytes(UART_NUM_0, line, len);
    } else {
        fputs(line, stdout);
    }

    // 2. RAM ring buffer
    write_to_ram_buf(line, len);

    if (s_initialized) {
        // 3. Flush buffer
        append_to_flush_buf(line, len);

        // 4. Immediate flush flag on error lines
        if (len > 0 && line[0] == 'E') {
            s_flush_needed = true;
        }
    }

    return len;
}

static void flush_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));  // 30 s
        log_manager_flush();
    }
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "Power-on (ESP_RST_POWERON)";
        case ESP_RST_EXT:       return "External reset (ESP_RST_EXT)";
        case ESP_RST_SW:        return "Software reset (ESP_RST_SW)";
        case ESP_RST_PANIC:     return "Panic/crash (ESP_RST_PANIC)";
        case ESP_RST_INT_WDT:   return "Interrupt watchdog (ESP_RST_INT_WDT)";
        case ESP_RST_TASK_WDT:  return "Task watchdog (ESP_RST_TASK_WDT)";
        case ESP_RST_WDT:       return "Other watchdog (ESP_RST_WDT)";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wakeup (ESP_RST_DEEPSLEEP)";
        case ESP_RST_BROWNOUT:  return "Brownout (ESP_RST_BROWNOUT)";
        case ESP_RST_SDIO:      return "SDIO (ESP_RST_SDIO)";
        default:                return "Unknown";
    }
}

static int read_session_index(void)
{
    FILE *f = fopen(LOG_IDX_FILE, "r");
    if (!f) return -1;
    int idx = -1;
    fscanf(f, "%d", &idx);
    fclose(f);
    return idx;
}

static void write_session_index(int idx)
{
    FILE *f = fopen(LOG_IDX_FILE, "w");
    if (!f) return;
    fprintf(f, "%d", idx);
    fclose(f);
}

static char *session_path(int n, char *buf, size_t buf_size)
{
    // 8.3 filename: "sess0.log" = 5+3 → valid without LFN
    snprintf(buf, buf_size, LOG_MOUNT_POINT "/sess%d.log", n);
    return buf;
}

// ============================================================================
// Public API
// ============================================================================

// Force-erase partition and remount with fresh FAT format
static esp_err_t reformat_fat(void)
{
    esp_vfs_fat_spiflash_unmount_rw_wl(LOG_MOUNT_POINT, s_wl_handle);
    s_wl_handle = WL_INVALID_HANDLE;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
        LOG_PARTITION_LABEL);
    if (!part) return ESP_ERR_NOT_FOUND;

    ESP_EARLY_LOGW(TAG, "Erasing log_store partition for clean format...");
    esp_err_t ret = esp_partition_erase_range(part, 0, part->size);
    if (ret != ESP_OK) return ret;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(
        LOG_MOUNT_POINT, LOG_PARTITION_LABEL, &mount_cfg, &s_wl_handle);
}

esp_err_t log_manager_init(void)
{
    // Install vprintf hook FIRST so the RAM ring buffer captures all logs,
    // even if FAT initialisation later fails.  FAT-side writes are gated by
    // s_initialized, which is set only after a successful mount.
    esp_log_set_vprintf(log_vprintf_hook);

    // Mount FAT partition with wear-levelling
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(
        LOG_MOUNT_POINT, LOG_PARTITION_LABEL, &mount_cfg, &s_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FAT mount failed: %s — live view only", esp_err_to_name(ret));
        return ret;
    }

    // Verify the filesystem is actually writable (stale/corrupt WL state
    // can cause f_mount to succeed but file creation to silently fail).
    {
        FILE *probe = fopen("/logs/probe", "w");
        if (!probe) {
            ESP_LOGW(TAG, "FAT not writable — reformatting...");
            ret = reformat_fat();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Reformat failed: %s — live view only", esp_err_to_name(ret));
                return ret;
            }
            ESP_LOGI(TAG, "Reformat OK");
        } else {
            fclose(probe);
            remove("/logs/probe");
        }
    }

    // Determine next session index
    int last = read_session_index();
    s_current_session = (last < 0) ? 0 : (last + 1) % LOG_MAX_SESSIONS;
    write_session_index(s_current_session);

    // Open (create/overwrite) current session file
    char path[64];
    session_path(s_current_session, path, sizeof(path));
    s_session_file = fopen(path, "w");
    if (!s_session_file) {
        ESP_LOGE(TAG, "Failed to open session file: %s — live view only", path);
        return ESP_FAIL;
    }

    // Write session header
    esp_reset_reason_t reason = esp_reset_reason();
    fprintf(s_session_file,
            "=== SESSION START ===\n"
            "Reset: %s\n"
            "IDF:   %s\n"
            "====================\n",
            reset_reason_str(reason),
            IDF_VER);
    fflush(s_session_file);
    fsync(fileno(s_session_file));  // commit header to FAT directory entry

    s_flush_sem = xSemaphoreCreateMutex();

    s_initialized = true;  // enables FAT flushing in the hook

    // Dedicated flush task (3KB stack — enough for FAT fwrite)
    xTaskCreate(flush_task, "log_flush", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "FAT mounted, sess%d.log created (reset: %s)",
             s_current_session, reset_reason_str(reason));

    return ESP_OK;
}

void log_manager_flush(void)
{
    if (s_session_file == NULL) return;
    if (s_flush_sem == NULL) return;

    // Serialize concurrent flush calls (timer cb + WiFi shutdown)
    if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(200)) != pdTRUE) return;

    portENTER_CRITICAL(&s_flush_mux);
    int len = s_flush_pos;
    if (len > 0) {
        memcpy(s_flush_local, s_flush_buf, len);
        s_flush_pos = 0;
    }
    portEXIT_CRITICAL(&s_flush_mux);

    if (len > 0) {
        fwrite(s_flush_local, 1, len, s_session_file);
        fflush(s_session_file);
        // fsync updates the FAT directory entry (file size) so stat() is
        // accurate after reboot and the file is readable by other openers.
        fsync(fileno(s_session_file));
    }
    s_flush_needed = false;

    xSemaphoreGive(s_flush_sem);
}

void log_manager_get_live(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;

    // Copy ring buffer directly into caller's buffer under spinlock
    portENTER_CRITICAL(&s_ram_mux);
    int len  = s_ram_len;
    int head = s_ram_head;
    int copy = (len < (int)(out_size - 1)) ? len : (int)(out_size - 1);
    for (int i = 0; i < copy; i++) {
        out[i] = s_ram_buf[(head + i) % LOG_RAM_BUF];
    }
    portEXIT_CRITICAL(&s_ram_mux);

    out[copy] = '\0';
}

esp_err_t log_manager_list_sessions(cJSON *arr)
{
    if (!arr) return ESP_ERR_INVALID_ARG;

    char path[64];
    for (int i = 0; i < LOG_MAX_SESSIONS; i++) {
        session_path(i, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) != 0) continue;

        cJSON *obj = cJSON_CreateObject();
        char name[32];
        snprintf(name, sizeof(name), "sess%d.log", i);
        cJSON_AddStringToObject(obj, "name", name);
        cJSON_AddNumberToObject(obj, "size", (double)st.st_size);
        cJSON_AddBoolToObject(obj, "current", (i == s_current_session));
        cJSON_AddItemToArray(arr, obj);
    }
    return ESP_OK;
}

esp_err_t log_manager_get_session(const char *name, httpd_req_t *req)
{
    if (!name || !req) return ESP_ERR_INVALID_ARG;

    // Validate name: only "sessN.log" pattern accepted (8.3 compliant)
    int n = -1;
    if (sscanf(name, "sess%d.log", &n) != 1 || n < 0 || n >= LOG_MAX_SESSIONS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session name");
        return ESP_FAIL;
    }

    char path[64];
    session_path(n, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t log_manager_clear(void)
{
    char path[64];
    for (int i = 0; i < LOG_MAX_SESSIONS; i++) {
        session_path(i, path, sizeof(path));
        remove(path);
    }
    remove(LOG_IDX_FILE);

    // Re-open a fresh session 0 if FAT is still mounted
    if (s_session_file) {
        fclose(s_session_file);
        s_session_file = NULL;
    }
    s_current_session = 0;
    write_session_index(0);

    session_path(0, path, sizeof(path));
    s_session_file = fopen(path, "w");
    if (s_session_file) {
        fprintf(s_session_file, "=== LOG CLEARED ===\n");
        fflush(s_session_file);
    }

    return ESP_OK;
}

// ============================================================================
// Log filter
// ============================================================================

static const char * const FILTERED_TAGS[] = {
    "BLE_TASK", "BLE_SERVICE", "BLE_HANDLERS",
    "WIFI_TASK",
    "NimBLE",
    NULL
};

void log_manager_set_filter(bool zigbee_only)
{
    esp_log_level_t level = zigbee_only ? ESP_LOG_NONE : ESP_LOG_INFO;
    for (int i = 0; FILTERED_TAGS[i] != NULL; i++) {
        esp_log_level_set(FILTERED_TAGS[i], level);
    }
    ESP_LOGI(TAG, "Log filter: %s", zigbee_only ? "Zigbee-only" : "all");
}

void log_manager_set_serial_interface(uint8_t iface)
{
    s_serial_iface = iface;
    // Log this message before the switch so it still goes to the old channel,
    // confirming the transition point in both channels.
    ESP_LOGI(TAG, "Serial interface: %s", iface == 1 ? "UART0" : "USB-JTAG/stdout");
}
