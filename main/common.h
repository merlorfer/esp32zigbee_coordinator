/**
 * @file common.h
 * @brief Common definitions for ESP32-C6 Zigbee Gateway
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Hardware Configuration
 * ============================================================================ */

#define GPIO_BUTTON_TOGGLE      GPIO_NUM_9
#define GPIO_LED_STATUS         GPIO_NUM_15

/* ============================================================================
 * Local XKC Sensor Configuration
 * ============================================================================ */

#define LOCAL_XKC_IEEE_ADDR     0x0000000000000001ULL  // Reserved address for local XKC sensor
#define LOCAL_XKC_ENDPOINT      1                       // Virtual endpoint
#define DEFAULT_XKC_GPIO_LOWER  4                       // Default GPIO for lower sensor
#define DEFAULT_XKC_GPIO_UPPER  5                       // Default GPIO for upper sensor

/* ============================================================================
 * Wi-Fi Configuration
 * ============================================================================ */

#define WIFI_SSID               "ESP32C6_AI_Test"
#define WIFI_PASSWORD           "12345678"
#define WIFI_CHANNEL            6
#define WIFI_MAX_CONNECTIONS    4
#define WIFI_AP_IP              "192.168.4.1"

/* ============================================================================
 * Zigbee Configuration
 * ============================================================================ */

#define MAX_DEVICES             10
#define ZIGBEE_PERMIT_JOIN_TIME 60
#define ZIGBEE_CMD_TIMEOUT_MS   5000
#define ZIGBEE_RETRY_COUNT      3
#define ZIGBEE_RETRY_DELAY_MS   2000

/* ============================================================================
 * Data Structure Limits
 * ============================================================================ */

#define MAX_DEVICE_NAME_LEN     32
#define MAX_MANUFACTURER_LEN    32
#define MAX_MODEL_LEN           32
#define MAX_TIME_PAIRS          5
#define MAX_ERROR_MSG_LEN       64

/* ============================================================================
 * Task Priorities
 * ============================================================================ */

#define TASK_PRIORITY_BUTTON    12
#define TASK_PRIORITY_ZIGBEE    10
#define TASK_PRIORITY_SCHEDULER 8
#define TASK_PRIORITY_WIFI      5
#define TASK_PRIORITY_BLE       5
#define TASK_PRIORITY_LED       3

/* ============================================================================
 * Task Stack Sizes
 * ============================================================================ */

#define TASK_STACK_BUTTON       2048
#define TASK_STACK_ZIGBEE       8192
#define TASK_STACK_SCHEDULER    4096
#define TASK_STACK_WIFI         6144  // Increased for sensor data (3 devices with large config)
#define TASK_STACK_BLE          6144  // Increased for sensor data (3 devices with large config)
#define TASK_STACK_LED          2048

/* ============================================================================
 * Event Group Bits
 * ============================================================================ */

#define EVENT_WIFI_MODE_BIT     BIT0
#define EVENT_ZIGBEE_MODE_BIT   BIT1
#define EVENT_RTC_INIT_BIT      BIT2
#define EVENT_ERROR_BIT         BIT3
#define EVENT_BLE_MODE_BIT      BIT4

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/**
 * @brief Time point structure (HH:MM)
 */
typedef struct {
    uint8_t hour;
    uint8_t minute;
} time_point_t;

/**
 * @brief Time pair for ON/OFF schedule
 */
typedef struct {
    time_point_t on_time;
    time_point_t off_time;
} time_pair_t;

/**
 * @brief Automation mode enumeration
 */
typedef enum {
    MODE_FIXED_TIME = 0,
    MODE_DELAY = 1
} automation_mode_t;

/**
 * @brief Device type enumeration
 */
typedef enum {
    DEVICE_TYPE_ON_OFF_LIGHT = 0,
    DEVICE_TYPE_TEMPERATURE_SENSOR = 1,
    DEVICE_TYPE_HUMIDITY_SENSOR = 2,
    DEVICE_TYPE_WATER_LEVEL_SENSOR = 3
} device_type_t;

/**
 * @brief Sensor configuration structure
 */
typedef struct {
    float lower_threshold;
    float upper_threshold;
    float lower_hysteresis;
    float upper_hysteresis;
    uint64_t lower_linked_device;  // IEEE addr or 0
    uint64_t upper_linked_device;  // IEEE addr or 0
    uint16_t timeout_seconds;      // Default: 60

    // Error handling (on sensor timeout)
    uint64_t error_linked_device;  // Device to switch on error (0 = none)
    bool error_action_on;          // true = ON on error, false = OFF on error

    // Report configuration (persisted, used by configure_sensor_reporting)
    uint16_t report_min_interval;  // Min interval in seconds (default 0)
    uint16_t report_max_interval;  // Max interval in seconds (default 10)
    int16_t report_change;         // Reportable change raw value (default 50 = 0.50 unit)

    // Delay settings (persisted):
    uint16_t lower_delay_seconds;  // Delay before ON for lower threshold (default: 0 = immediate)
    uint16_t upper_delay_seconds;  // Delay before ON for upper threshold (default: 0 = immediate)

    // Runtime state (not persisted):
    bool lower_active;
    bool upper_active;
    uint32_t last_lower_action;
    uint32_t last_upper_action;
    // Delay runtime state:
    bool lower_delay_pending;      // Delay timer running
    bool upper_delay_pending;
    uint32_t lower_delay_start;    // When delay timer started
    uint32_t upper_delay_start;
} sensor_config_t;

/**
 * @brief Sensor reading structure (runtime only, not saved to NVS)
 */
typedef struct {
    int16_t raw_value;
    float converted_value;
    uint32_t last_update;
    bool valid;
    uint8_t battery_percent;       // 0-100, 0xFF = not available
    uint8_t battery_voltage_100mv; // Voltage in 100mV units, 0xFF = not available
} sensor_reading_t;

/**
 * @brief Device configuration structure
 */
typedef struct {
    // Zigbee identification
    uint64_t ieee_addr;
    uint8_t endpoint;

    // Device type
    device_type_t device_type;

    // Device information (from Zigbee attributes)
    char manufacturer[MAX_MANUFACTURER_LEN];
    char model[MAX_MODEL_LEN];

    // User settings
    char custom_name[MAX_DEVICE_NAME_LEN];
    bool enabled;

    // Automation mode (only for ON_OFF_LIGHT devices)
    automation_mode_t mode;

    // Fixed time mode
    uint8_t time_pair_count;
    time_pair_t time_pairs[MAX_TIME_PAIRS];

    // Delay mode (cyclic operation) - 3-phase: OFF1 → ON → OFF2
    uint16_t delay_off1_minutes;        // OFF phase before ON (can be 0)
    uint16_t delay_duration_minutes;    // ON phase duration
    uint16_t delay_off2_minutes;        // OFF phase after ON (can be 0)
    uint32_t delay_cycle_start;         // Cycle start timestamp

    // Sensor configuration (only for sensor devices)
    sensor_config_t sensor;

    // Sensor reading (runtime only, not saved to NVS)
    sensor_reading_t reading;
} device_config_t;

/**
 * @brief Device error structure
 */
typedef struct {
    uint64_t ieee_addr;
    uint32_t timestamp;
    char error_message[MAX_ERROR_MSG_LEN];
    bool active;
} device_error_t;

/**
 * @brief Global configuration structure
 */
typedef struct {
    bool wifi_on_behavior;    // true=maintain state, false=power off on WiFi AP start
    uint8_t device_count;
    bool rtc_initialized;
    uint32_t last_rtc_set;

    // Local XKC sensor configuration (persisted to NVS)
    bool local_xkc_enabled;       // Toggle: enable local XKC water level sensor
    uint8_t local_xkc_gpio_lower; // GPIO pin for lower sensor (default: DEFAULT_XKC_GPIO_LOWER)
    uint8_t local_xkc_gpio_upper; // GPIO pin for upper sensor (default: DEFAULT_XKC_GPIO_UPPER)
} global_config_t;

/**
 * @brief LED state enumeration
 */
typedef enum {
    LED_STATE_NORMAL,         // 1 sec blink - automation mode active
    LED_STATE_WIFI_ACTIVE,    // Solid ON - Wi-Fi AP active (also used for setup mode)
    LED_STATE_BLE_ACTIVE,     // Slow 2 sec blink - BLE configuration mode
    LED_STATE_PAIRING,        // Fast blink 0.25 sec - Zigbee pairing mode
    LED_STATE_ERROR           // 3x fast blink, pause - error occurred
} led_state_t;

/**
 * @brief Command type for Zigbee
 */
typedef enum {
    CMD_ON,
    CMD_OFF,
    CMD_TOGGLE
} zigbee_cmd_type_t;

/**
 * @brief Command queue message
 */
typedef struct {
    uint64_t ieee_addr;
    uint8_t endpoint;
    zigbee_cmd_type_t cmd;
} cmd_queue_msg_t;

/**
 * @brief Error queue message
 */
typedef struct {
    uint64_t ieee_addr;
    char message[MAX_ERROR_MSG_LEN];
} error_queue_msg_t;

/**
 * @brief Sensor data queue message
 */
typedef struct {
    uint64_t ieee_addr;
    uint8_t endpoint;
    uint16_t cluster_id;
    int16_t raw_value;
    uint32_t timestamp;
} sensor_data_msg_t;

/* ============================================================================
 * Global Variables (extern declarations)
 * ============================================================================ */

extern EventGroupHandle_t g_event_group;
extern QueueHandle_t g_cmd_queue;
extern QueueHandle_t g_led_queue;
extern QueueHandle_t g_error_queue;
extern QueueHandle_t g_sensor_data_queue;
extern SemaphoreHandle_t g_nvs_mutex;
extern SemaphoreHandle_t g_device_mutex;

#ifdef __cplusplus
}
#endif
