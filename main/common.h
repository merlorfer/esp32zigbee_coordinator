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
 * Wi-Fi Configuration
 * ============================================================================ */

#define WIFI_SSID               "ESP32C6_AI_Test"
#define WIFI_PASSWORD           "12345678"
#define WIFI_CHANNEL            1
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
#define TASK_PRIORITY_LED       3

/* ============================================================================
 * Task Stack Sizes
 * ============================================================================ */

#define TASK_STACK_BUTTON       2048
#define TASK_STACK_ZIGBEE       8192
#define TASK_STACK_SCHEDULER    4096
#define TASK_STACK_WIFI         4096
#define TASK_STACK_LED          2048

/* ============================================================================
 * Event Group Bits
 * ============================================================================ */

#define EVENT_WIFI_MODE_BIT     BIT0
#define EVENT_ZIGBEE_MODE_BIT   BIT1
#define EVENT_RTC_INIT_BIT      BIT2
#define EVENT_ERROR_BIT         BIT3

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
 * @brief Device configuration structure
 */
typedef struct {
    // Zigbee identification
    uint64_t ieee_addr;
    uint8_t endpoint;

    // Device information (from Zigbee attributes)
    char manufacturer[MAX_MANUFACTURER_LEN];
    char model[MAX_MODEL_LEN];

    // User settings
    char custom_name[MAX_DEVICE_NAME_LEN];
    bool enabled;

    // Automation mode
    automation_mode_t mode;

    // Fixed time mode
    uint8_t time_pair_count;
    time_pair_t time_pairs[MAX_TIME_PAIRS];

    // Delay mode (cyclic operation)
    uint16_t delay_on_minutes;
    uint16_t delay_duration_minutes;
    uint32_t delay_cycle_start;

    // State tracking
    bool current_state;
    uint32_t last_command_timestamp;
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
} global_config_t;

/**
 * @brief LED state enumeration
 */
typedef enum {
    LED_STATE_NORMAL,         // 1 sec blink - automation mode active
    LED_STATE_WIFI_ACTIVE,    // Solid ON - Wi-Fi AP active
    LED_STATE_RTC_NOT_SET,    // Fast blink 0.5 sec - RTC not initialized
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

/* ============================================================================
 * Global Variables (extern declarations)
 * ============================================================================ */

extern EventGroupHandle_t g_event_group;
extern QueueHandle_t g_cmd_queue;
extern QueueHandle_t g_led_queue;
extern QueueHandle_t g_error_queue;
extern SemaphoreHandle_t g_nvs_mutex;
extern SemaphoreHandle_t g_device_mutex;

#ifdef __cplusplus
}
#endif
