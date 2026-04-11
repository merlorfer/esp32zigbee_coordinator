/**
 * @file rules_engine.h
 * @brief ESPEasy-style rules engine for event-driven automation
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_RULES           20
#define MAX_RULE_ACTIONS    6
#define MAX_CONDITIONS      6
#define MAX_RULE_TIMERS     8
#define MAX_RULE_VARIABLES  8
#define MAX_RULES_TEXT_SIZE 4096

/* ============================================================================
 * Enumerations
 * ============================================================================ */

typedef enum {
    EVT_SENSOR,
    EVT_TIMER,
    EVT_BOOT,
    EVT_VAR_CHANGED,
    EVT_TIME,
} rule_event_type_t;

typedef enum {
    OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE
} rule_operator_t;

typedef enum {
    VAL_SENSOR,
    VAL_VARIABLE,
    VAL_HOUR,
    VAL_MINUTE,
    VAL_UPTIME,
    VAL_LITERAL,
} rule_value_type_t;

typedef enum {
    ACT_DEVICE_ON,
    ACT_DEVICE_OFF,
    ACT_DEVICE_TOGGLE,
    ACT_TIMER_SET,
    ACT_TIMER_CANCEL,
    ACT_SET_VAR,
} rule_action_type_t;

/* ============================================================================
 * Data Structures
 * ============================================================================ */

typedef struct {
    rule_value_type_t type;
    union {
        uint8_t sensor_index;
        uint8_t var_index;
        float literal;
    };
} rule_value_t;

typedef struct {
    rule_value_t left;
    rule_operator_t op;
    rule_value_t right;
    bool negated;
} rule_condition_t;

typedef struct {
    rule_action_type_t type;
    uint8_t target_index;
    float value;
    // Math expression for ACT_SET_VAR: result = expr_left [expr_op expr_right]
    rule_value_t expr_left;
    int8_t expr_op;           // 0=none (use value), '+'=add, '-'=subtract
    rule_value_t expr_right;
} rule_action_t;

typedef struct {
    rule_event_type_t event_type;
    uint8_t event_param;
    uint16_t event_time;          // HH*60+MM for EVT_TIME

    bool has_condition;
    rule_condition_t conditions[MAX_CONDITIONS];
    uint8_t condition_count;
    bool condition_logic_or;      // false=AND, true=OR

    rule_action_t then_actions[MAX_RULE_ACTIONS];
    uint8_t then_count;

    rule_action_t else_actions[MAX_RULE_ACTIONS];
    uint8_t else_count;

    rule_action_t post_actions[MAX_RULE_ACTIONS];  // actions after endif (always run)
    uint8_t post_count;
} rule_t;

typedef struct {
    bool active;
    int32_t remaining_seconds;
} rule_timer_t;

typedef struct {
    bool  persist;
    float default_value;
} rule_var_config_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

esp_err_t rules_engine_init(void);
esp_err_t rules_engine_load_text(const char *text);
const char* rules_engine_get_text(void);
esp_err_t rules_engine_save(void);

void rules_engine_on_sensor_data(uint8_t device_index, float value);
void rules_engine_on_timer(uint8_t timer_id);
void rules_engine_on_boot(void);
void rules_engine_on_time_tick(uint8_t hour, uint8_t minute);

float rules_engine_get_var(uint8_t index);
void rules_engine_set_var(uint8_t index, float value);

void rules_engine_timer_tick(void);

uint8_t rules_engine_get_rule_count(void);
void rules_engine_get_timer_state(uint8_t index, bool *active, int32_t *remaining);
const char* rules_engine_get_parse_error(void);

void rules_engine_set_enabled(bool enabled);
bool rules_engine_get_enabled(void);

void rules_engine_set_var_config(uint8_t index, bool persist, float default_value);
void rules_engine_get_var_config(uint8_t index, bool *persist, float *default_value);
esp_err_t rules_engine_save_var_config(void);
esp_err_t rules_engine_reset(void);

/**
 * @brief Execute a single rules engine command string directly
 * Supports: "set varN value", "timerSet N seconds", "timerCancel N",
 *           "on DeviceName", "off DeviceName", "toggle DeviceName"
 * @param cmd_str Command string to execute
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on parse failure
 */
esp_err_t rules_engine_exec_command(const char *cmd_str);

#ifdef __cplusplus
}
#endif
