/**
 * @file rules_engine.c
 * @brief ESPEasy-style rules engine - parser, evaluator, executor
 */

#include "rules_engine.h"
#include "common.h"
#include "device_manager.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <math.h>

static const char *TAG = "RULES_ENGINE";

extern QueueHandle_t g_cmd_queue;

/* Forward declaration */
static void rules_engine_on_var_changed(uint8_t var_index);

/* ============================================================================
 * Static State
 * ============================================================================ */

static rule_t s_rules[MAX_RULES];
static uint8_t s_rule_count = 0;

static float s_variables[MAX_RULE_VARIABLES];
static bool  s_var_persist[MAX_RULE_VARIABLES];   // default: true
static float s_var_defaults[MAX_RULE_VARIABLES];  // default: 0.0
static rule_timer_t s_timers[MAX_RULE_TIMERS];
static float s_sensor_values[MAX_DEVICES];

static char s_rules_text[MAX_RULES_TEXT_SIZE];
static char s_parse_error[128];

static uint32_t s_boot_time;
static uint16_t s_last_time_tick;  // HH*60+MM of last processed tick

/* ============================================================================
 * Helper: Find device index by custom_name
 * ============================================================================ */

static int find_device_by_name(const char *name)
{
    uint8_t count = device_manager_get_count();
    for (uint8_t i = 0; i < count; i++) {
        device_config_t dev;
        if (device_manager_get_by_index(i, &dev) == ESP_OK) {
            if (strlen(dev.custom_name) > 0 && strcmp(dev.custom_name, name) == 0) {
                return i;
            }
        }
    }
    return -1;
}

/* ============================================================================
 * Parser Helpers
 * ============================================================================ */

static void skip_whitespace(const char **p)
{
    while (**p == ' ' || **p == '\t') (*p)++;
}

static bool parse_word(const char **p, char *out, size_t max_len)
{
    skip_whitespace(p);
    size_t i = 0;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n' && **p != '\r' && i < max_len - 1) {
        out[i++] = **p;
        (*p)++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool parse_number(const char **p, float *out)
{
    skip_whitespace(p);
    char *end;
    *out = strtof(*p, &end);
    if (end == *p) return false;
    *p = end;
    return true;
}

static bool strcasecmp_word(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Parse a value reference: [SensorName], [var1], [hour], [minute], [uptime], or literal number */
static bool parse_value(const char **p, rule_value_t *val, int line_num)
{
    skip_whitespace(p);

    if (**p == '[') {
        (*p)++;  // skip '['
        char name[MAX_DEVICE_NAME_LEN];
        size_t i = 0;
        while (**p && **p != ']' && i < sizeof(name) - 1) {
            name[i++] = **p;
            (*p)++;
        }
        name[i] = '\0';
        if (**p == ']') (*p)++;

        if (strcasecmp_word(name, "hour")) {
            val->type = VAL_HOUR;
        } else if (strcasecmp_word(name, "minute")) {
            val->type = VAL_MINUTE;
        } else if (strcasecmp_word(name, "uptime")) {
            val->type = VAL_UPTIME;
        } else if (strncasecmp(name, "var", 3) == 0 && strlen(name) == 4 && name[3] >= '1' && name[3] <= '8') {
            val->type = VAL_VARIABLE;
            val->var_index = name[3] - '1';
        } else {
            int idx = find_device_by_name(name);
            if (idx < 0) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: unknown device '%s'", line_num, name);
                return false;
            }
            val->type = VAL_SENSOR;
            val->sensor_index = (uint8_t)idx;
        }
        return true;
    }

    // Try parsing as literal number
    float num;
    if (parse_number(p, &num)) {
        val->type = VAL_LITERAL;
        val->literal = num;
        return true;
    }

    snprintf(s_parse_error, sizeof(s_parse_error),
             "Line %d: expected value ([name] or number)", line_num);
    return false;
}

/* Parse operator: <, >, <=, >=, =, != */
static bool parse_operator(const char **p, rule_operator_t *op)
{
    skip_whitespace(p);

    if (**p == '<') {
        (*p)++;
        if (**p == '=') { (*p)++; *op = OP_LE; }
        else { *op = OP_LT; }
        return true;
    }
    if (**p == '>') {
        (*p)++;
        if (**p == '=') { (*p)++; *op = OP_GE; }
        else { *op = OP_GT; }
        return true;
    }
    if (**p == '!') {
        (*p)++;
        if (**p == '=') { (*p)++; *op = OP_NE; return true; }
        return false;
    }
    if (**p == '=') {
        (*p)++;
        *op = OP_EQ;
        return true;
    }

    return false;
}

/* Parse a single condition: [left] op right, optionally preceded by "not" */
static bool parse_single_condition(const char **p, rule_condition_t *cond, int line_num)
{
    skip_whitespace(p);
    cond->negated = false;

    // Check for "not" prefix
    const char *save = *p;
    char word[16];
    if (parse_word(p, word, sizeof(word)) && strcasecmp_word(word, "not")) {
        cond->negated = true;
    } else {
        *p = save;
    }

    if (!parse_value(p, &cond->left, line_num)) return false;
    if (!parse_operator(p, &cond->op)) {
        snprintf(s_parse_error, sizeof(s_parse_error),
                 "Line %d: expected operator", line_num);
        return false;
    }
    if (!parse_value(p, &cond->right, line_num)) return false;

    return true;
}

/* Parse an action line: on/off/toggle Device, timerSet N S, timerCancel N, set varN V */
static bool parse_action(const char *line, rule_action_t *act, int line_num)
{
    const char *p = line;
    char word[MAX_DEVICE_NAME_LEN];

    if (!parse_word(&p, word, sizeof(word))) return false;

    if (strcasecmp_word(word, "on")) {
        char name[MAX_DEVICE_NAME_LEN];
        if (!parse_word(&p, name, sizeof(name))) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: 'on' requires device name", line_num);
            return false;
        }
        int idx = find_device_by_name(name);
        if (idx < 0) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: unknown device '%s'", line_num, name);
            return false;
        }
        act->type = ACT_DEVICE_ON;
        act->target_index = (uint8_t)idx;
        return true;
    }

    if (strcasecmp_word(word, "off")) {
        char name[MAX_DEVICE_NAME_LEN];
        if (!parse_word(&p, name, sizeof(name))) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: 'off' requires device name", line_num);
            return false;
        }
        int idx = find_device_by_name(name);
        if (idx < 0) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: unknown device '%s'", line_num, name);
            return false;
        }
        act->type = ACT_DEVICE_OFF;
        act->target_index = (uint8_t)idx;
        return true;
    }

    if (strcasecmp_word(word, "toggle")) {
        char name[MAX_DEVICE_NAME_LEN];
        if (!parse_word(&p, name, sizeof(name))) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: 'toggle' requires device name", line_num);
            return false;
        }
        int idx = find_device_by_name(name);
        if (idx < 0) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: unknown device '%s'", line_num, name);
            return false;
        }
        act->type = ACT_DEVICE_TOGGLE;
        act->target_index = (uint8_t)idx;
        return true;
    }

    if (strcasecmp_word(word, "timerSet") || strcasecmp_word(word, "timerset")) {
        float timer_id;
        if (!parse_number(&p, &timer_id)) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: timerSet requires <id> <value>", line_num);
            return false;
        }
        if ((int)timer_id < 1 || (int)timer_id > MAX_RULE_TIMERS) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: timer id must be 1-%d", line_num, MAX_RULE_TIMERS);
            return false;
        }
        act->type = ACT_TIMER_SET;
        act->target_index = (uint8_t)timer_id - 1;
        act->expr_op = 0;

        // Parse seconds as value expression (supports [var], literals, +/-)
        if (!parse_value(&p, &act->expr_left, line_num)) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: timerSet requires seconds value", line_num);
            return false;
        }

        // Optional +/- operator
        skip_whitespace(&p);
        if (*p == '+' || *p == '-') {
            act->expr_op = *p;
            p++;
            if (!parse_value(&p, &act->expr_right, line_num)) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: expected value after '%c'", line_num, act->expr_op);
                return false;
            }
        }

        // Simple literal: also store in value for backward compat
        if (act->expr_op == 0 && act->expr_left.type == VAL_LITERAL) {
            act->value = act->expr_left.literal;
        }
        return true;
    }

    if (strcasecmp_word(word, "timerCancel") || strcasecmp_word(word, "timercancel")) {
        float timer_id;
        if (!parse_number(&p, &timer_id)) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: timerCancel requires <id>", line_num);
            return false;
        }
        if ((int)timer_id < 1 || (int)timer_id > MAX_RULE_TIMERS) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: timer id must be 1-%d", line_num, MAX_RULE_TIMERS);
            return false;
        }
        act->type = ACT_TIMER_CANCEL;
        act->target_index = (uint8_t)timer_id - 1;
        return true;
    }

    if (strcasecmp_word(word, "set")) {
        char varname[16];
        if (!parse_word(&p, varname, sizeof(varname))) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: 'set' requires var name", line_num);
            return false;
        }
        if (strncasecmp(varname, "var", 3) != 0 || strlen(varname) != 4 ||
            varname[3] < '1' || varname[3] > '8') {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: expected var1..var8, got '%s'", line_num, varname);
            return false;
        }
        act->type = ACT_SET_VAR;
        act->target_index = varname[3] - '1';
        act->expr_op = 0;

        // Parse value expression: literal, [ref], or [ref] +/- value
        if (!parse_value(&p, &act->expr_left, line_num)) {
            snprintf(s_parse_error, sizeof(s_parse_error),
                     "Line %d: set requires a value or expression", line_num);
            return false;
        }

        // Check for optional +/- operator
        skip_whitespace(&p);
        if (*p == '+' || *p == '-') {
            act->expr_op = *p;
            p++;
            if (!parse_value(&p, &act->expr_right, line_num)) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: expected value after '%c'", line_num, act->expr_op);
                return false;
            }
        }

        // For simple literals with no operator, also store in value for backward compat
        if (act->expr_op == 0 && act->expr_left.type == VAL_LITERAL) {
            act->value = act->expr_left.literal;
        }
        return true;
    }

    snprintf(s_parse_error, sizeof(s_parse_error),
             "Line %d: unknown action '%s'", line_num, word);
    return false;
}

/* ============================================================================
 * Main Parser
 * ============================================================================ */

static esp_err_t parse_rules(const char *text)
{
    s_rule_count = 0;
    s_parse_error[0] = '\0';

    if (text == NULL || text[0] == '\0') {
        ESP_LOGI(TAG, "Empty rules text, no rules loaded");
        return ESP_OK;
    }

    // State machine
    enum { STATE_IDLE, STATE_IN_RULE, STATE_IN_IF, STATE_IN_ELSE } state = STATE_IDLE;
    rule_t *cur = NULL;
    int line_num = 0;

    // Process line by line
    const char *p = text;
    while (*p) {
        // Extract one line
        char line[256];
        size_t li = 0;
        while (*p && *p != '\n' && *p != '\r' && li < sizeof(line) - 1) {
            line[li++] = *p++;
        }
        line[li] = '\0';
        // Skip newline
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        line_num++;

        // Trim leading whitespace
        const char *lp = line;
        skip_whitespace(&lp);

        // Skip empty lines and comments
        if (*lp == '\0' || (lp[0] == '/' && lp[1] == '/')) continue;

        // Lowercase first word for keyword matching
        char keyword[32];
        const char *kp = lp;
        parse_word(&kp, keyword, sizeof(keyword));

        // ---- STATE_IDLE: expect "on ... do" ----
        if (state == STATE_IDLE) {
            if (!strcasecmp_word(keyword, "on")) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: expected 'on ... do'", line_num);
                return ESP_ERR_INVALID_ARG;
            }

            if (s_rule_count >= MAX_RULES) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: max %d rules exceeded", line_num, MAX_RULES);
                return ESP_ERR_NO_MEM;
            }

            cur = &s_rules[s_rule_count];
            memset(cur, 0, sizeof(rule_t));

            // Parse event type: "on <event> do"
            char event_name[MAX_DEVICE_NAME_LEN];
            if (!parse_word(&kp, event_name, sizeof(event_name))) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: expected event name after 'on'", line_num);
                return ESP_ERR_INVALID_ARG;
            }

            if (strcasecmp_word(event_name, "boot")) {
                cur->event_type = EVT_BOOT;
            } else if (strncasecmp(event_name, "timer", 5) == 0) {
                // "on timer N do" - timer number is next word
                // But first check if number is part of "timer" (e.g. "timer1")
                const char *num_part = event_name + 5;
                float timer_id;
                if (*num_part != '\0') {
                    timer_id = strtof(num_part, NULL);
                } else {
                    // Next word should be the number
                    char num_word[8];
                    if (!parse_word(&kp, num_word, sizeof(num_word))) {
                        snprintf(s_parse_error, sizeof(s_parse_error),
                                 "Line %d: 'on timer' requires number", line_num);
                        return ESP_ERR_INVALID_ARG;
                    }
                    timer_id = strtof(num_word, NULL);
                }
                if ((int)timer_id < 1 || (int)timer_id > MAX_RULE_TIMERS) {
                    snprintf(s_parse_error, sizeof(s_parse_error),
                             "Line %d: timer id must be 1-%d", line_num, MAX_RULE_TIMERS);
                    return ESP_ERR_INVALID_ARG;
                }
                cur->event_type = EVT_TIMER;
                cur->event_param = (uint8_t)timer_id - 1;
            } else if (strcasecmp_word(event_name, "time")) {
                // "on time HH:MM do"
                char time_str[8];
                if (!parse_word(&kp, time_str, sizeof(time_str))) {
                    snprintf(s_parse_error, sizeof(s_parse_error),
                             "Line %d: 'on time' requires HH:MM", line_num);
                    return ESP_ERR_INVALID_ARG;
                }
                int hh, mm;
                if (sscanf(time_str, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                    snprintf(s_parse_error, sizeof(s_parse_error),
                             "Line %d: invalid time '%s'", line_num, time_str);
                    return ESP_ERR_INVALID_ARG;
                }
                cur->event_type = EVT_TIME;
                cur->event_time = hh * 60 + mm;
            } else if (strncasecmp(event_name, "var", 3) == 0 && strlen(event_name) == 4 &&
                       event_name[3] >= '1' && event_name[3] <= '8') {
                cur->event_type = EVT_VAR_CHANGED;
                cur->event_param = event_name[3] - '1';
            } else {
                // Sensor name
                int idx = find_device_by_name(event_name);
                if (idx < 0) {
                    snprintf(s_parse_error, sizeof(s_parse_error),
                             "Line %d: unknown device '%s'", line_num, event_name);
                    return ESP_ERR_INVALID_ARG;
                }
                cur->event_type = EVT_SENSOR;
                cur->event_param = (uint8_t)idx;
            }

            // Expect "do" at end
            char do_word[8];
            if (!parse_word(&kp, do_word, sizeof(do_word)) || !strcasecmp_word(do_word, "do")) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: expected 'do' at end of 'on' line", line_num);
                return ESP_ERR_INVALID_ARG;
            }

            state = STATE_IN_RULE;
            continue;
        }

        // ---- STATE_IN_RULE, STATE_IN_IF, STATE_IN_ELSE ----
        if (strcasecmp_word(keyword, "endon")) {
            if (state == STATE_IN_IF || state == STATE_IN_ELSE) {
                // Implicit endif before endon
            }
            s_rule_count++;
            state = STATE_IDLE;
            cur = NULL;
            continue;
        }

        if (strcasecmp_word(keyword, "endif")) {
            if (state != STATE_IN_IF && state != STATE_IN_ELSE) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: 'endif' without matching 'if'", line_num);
                return ESP_ERR_INVALID_ARG;
            }
            state = STATE_IN_RULE;
            continue;
        }

        if (strcasecmp_word(keyword, "else")) {
            if (state != STATE_IN_IF) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: 'else' without matching 'if'", line_num);
                return ESP_ERR_INVALID_ARG;
            }
            state = STATE_IN_ELSE;
            continue;
        }

        if (strcasecmp_word(keyword, "if")) {
            if (state != STATE_IN_RULE) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: nested 'if' not supported", line_num);
                return ESP_ERR_INVALID_ARG;
            }

            cur->has_condition = true;
            cur->condition_count = 0;
            cur->condition_logic_or = false;

            // Parse first condition
            if (cur->condition_count >= MAX_CONDITIONS) {
                snprintf(s_parse_error, sizeof(s_parse_error),
                         "Line %d: max %d conditions exceeded", line_num, MAX_CONDITIONS);
                return ESP_ERR_INVALID_ARG;
            }
            if (!parse_single_condition(&kp, &cur->conditions[cur->condition_count], line_num)) {
                return ESP_ERR_INVALID_ARG;
            }
            cur->condition_count++;

            // Parse additional conditions with and/or
            while (true) {
                skip_whitespace(&kp);
                if (*kp == '\0' || *kp == '\n' || *kp == '\r') break;

                char logic[8];
                const char *save = kp;
                if (!parse_word(&kp, logic, sizeof(logic))) break;

                if (strcasecmp_word(logic, "and")) {
                    cur->condition_logic_or = false;
                } else if (strcasecmp_word(logic, "or")) {
                    cur->condition_logic_or = true;
                } else {
                    kp = save;
                    break;
                }

                if (cur->condition_count >= MAX_CONDITIONS) {
                    snprintf(s_parse_error, sizeof(s_parse_error),
                             "Line %d: max %d conditions exceeded", line_num, MAX_CONDITIONS);
                    return ESP_ERR_INVALID_ARG;
                }
                if (!parse_single_condition(&kp, &cur->conditions[cur->condition_count], line_num)) {
                    return ESP_ERR_INVALID_ARG;
                }
                cur->condition_count++;
            }

            state = STATE_IN_IF;
            continue;
        }

        // Action line
        if (state == STATE_IN_RULE || state == STATE_IN_IF || state == STATE_IN_ELSE) {
            rule_action_t act;
            memset(&act, 0, sizeof(act));
            if (!parse_action(lp, &act, line_num)) {
                return ESP_ERR_INVALID_ARG;
            }

            if (state == STATE_IN_ELSE) {
                if (cur->else_count >= MAX_RULE_ACTIONS) {
                    snprintf(s_parse_error, sizeof(s_parse_error),
                             "Line %d: max %d else-actions exceeded", line_num, MAX_RULE_ACTIONS);
                    return ESP_ERR_INVALID_ARG;
                }
                cur->else_actions[cur->else_count++] = act;
            } else {
                if (cur->then_count >= MAX_RULE_ACTIONS) {
                    snprintf(s_parse_error, sizeof(s_parse_error),
                             "Line %d: max %d then-actions exceeded", line_num, MAX_RULE_ACTIONS);
                    return ESP_ERR_INVALID_ARG;
                }
                cur->then_actions[cur->then_count++] = act;
            }
            continue;
        }
    }

    if (state != STATE_IDLE) {
        snprintf(s_parse_error, sizeof(s_parse_error),
                 "Unexpected end of rules: missing 'endon'");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Parsed %d rules successfully", s_rule_count);
    return ESP_OK;
}

/* ============================================================================
 * Evaluator
 * ============================================================================ */

static float resolve_value(const rule_value_t *val)
{
    switch (val->type) {
        case VAL_SENSOR:
            if (val->sensor_index < MAX_DEVICES) {
                return s_sensor_values[val->sensor_index];
            }
            return 0.0f;
        case VAL_VARIABLE:
            if (val->var_index < MAX_RULE_VARIABLES) {
                return s_variables[val->var_index];
            }
            return 0.0f;
        case VAL_HOUR: {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            return (float)tm.tm_hour;
        }
        case VAL_MINUTE: {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            return (float)tm.tm_min;
        }
        case VAL_UPTIME: {
            time_t now = time(NULL);
            return (float)((uint32_t)(now - s_boot_time) / 60);
        }
        case VAL_LITERAL:
            return val->literal;
    }
    return 0.0f;
}

static bool eval_condition(const rule_condition_t *cond)
{
    float left = resolve_value(&cond->left);
    float right = resolve_value(&cond->right);
    bool result;

    switch (cond->op) {
        case OP_LT: result = (left < right); break;
        case OP_GT: result = (left > right); break;
        case OP_LE: result = (left <= right); break;
        case OP_GE: result = (left >= right); break;
        case OP_EQ: result = (fabsf(left - right) < 0.001f); break;
        case OP_NE: result = (fabsf(left - right) >= 0.001f); break;
        default: result = false; break;
    }

    return cond->negated ? !result : result;
}

static bool eval_conditions(const rule_t *rule)
{
    if (!rule->has_condition || rule->condition_count == 0) {
        return true;  // No condition = always true
    }

    if (rule->condition_logic_or) {
        // OR logic
        for (uint8_t i = 0; i < rule->condition_count; i++) {
            if (eval_condition(&rule->conditions[i])) return true;
        }
        return false;
    } else {
        // AND logic
        for (uint8_t i = 0; i < rule->condition_count; i++) {
            if (!eval_condition(&rule->conditions[i])) return false;
        }
        return true;
    }
}

/* ============================================================================
 * Executor
 * ============================================================================ */

static void execute_action(const rule_action_t *act)
{
    switch (act->type) {
        case ACT_DEVICE_ON:
        case ACT_DEVICE_OFF:
        case ACT_DEVICE_TOGGLE: {
            device_config_t dev;
            if (device_manager_get_by_index(act->target_index, &dev) != ESP_OK) {
                ESP_LOGW(TAG, "Rule action: device index %d not found", act->target_index);
                return;
            }
            zigbee_cmd_type_t cmd;
            if (act->type == ACT_DEVICE_ON) cmd = CMD_ON;
            else if (act->type == ACT_DEVICE_OFF) cmd = CMD_OFF;
            else cmd = CMD_TOGGLE;

            cmd_queue_msg_t msg = {
                .ieee_addr = dev.ieee_addr,
                .endpoint = dev.endpoint,
                .cmd = cmd
            };
            xQueueSend(g_cmd_queue, &msg, pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "Rule action: %s %s",
                     act->type == ACT_DEVICE_ON ? "ON" :
                     act->type == ACT_DEVICE_OFF ? "OFF" : "TOGGLE",
                     dev.custom_name);
            break;
        }

        case ACT_TIMER_SET:
            if (act->target_index < MAX_RULE_TIMERS) {
                float secs;
                if (act->expr_op == 0 && act->expr_left.type == VAL_LITERAL) {
                    secs = act->value;
                } else {
                    secs = resolve_value(&act->expr_left);
                    if (act->expr_op == '+') secs += resolve_value(&act->expr_right);
                    else if (act->expr_op == '-') secs -= resolve_value(&act->expr_right);
                }
                s_timers[act->target_index].active = true;
                s_timers[act->target_index].remaining_seconds = (int32_t)secs;
                ESP_LOGI(TAG, "Rule action: timerSet %d %d sec",
                         act->target_index + 1, (int)secs);
            }
            break;

        case ACT_TIMER_CANCEL:
            if (act->target_index < MAX_RULE_TIMERS) {
                s_timers[act->target_index].active = false;
                s_timers[act->target_index].remaining_seconds = 0;
                ESP_LOGI(TAG, "Rule action: timerCancel %d", act->target_index + 1);
            }
            break;

        case ACT_SET_VAR:
            if (act->target_index < MAX_RULE_VARIABLES) {
                float new_val;
                if (act->expr_op == 0 && act->expr_left.type == VAL_LITERAL) {
                    // Simple literal assignment
                    new_val = act->value;
                } else {
                    // Expression evaluation
                    new_val = resolve_value(&act->expr_left);
                    if (act->expr_op == '+') {
                        new_val += resolve_value(&act->expr_right);
                    } else if (act->expr_op == '-') {
                        new_val -= resolve_value(&act->expr_right);
                    }
                }
                float old_val = s_variables[act->target_index];
                s_variables[act->target_index] = new_val;
                ESP_LOGI(TAG, "Rule action: set var%d = %.2f", act->target_index + 1, new_val);
                // Trigger var changed event if value actually changed
                if (fabsf(old_val - new_val) >= 0.001f) {
                    rules_engine_on_var_changed(act->target_index);
                }
            }
            break;
    }
}

static void execute_actions(const rule_action_t *actions, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        execute_action(&actions[i]);
    }
}

/* Internal var changed handler (avoids infinite recursion with depth limit) */
static uint8_t s_event_depth = 0;
#define MAX_EVENT_DEPTH 3

static void rules_engine_on_var_changed(uint8_t var_index)
{
    if (s_event_depth >= MAX_EVENT_DEPTH) {
        ESP_LOGW(TAG, "Max event depth reached, skipping var%d trigger", var_index + 1);
        return;
    }
    s_event_depth++;

    for (uint8_t i = 0; i < s_rule_count; i++) {
        rule_t *rule = &s_rules[i];
        if (rule->event_type == EVT_VAR_CHANGED && rule->event_param == var_index) {
            if (eval_conditions(rule)) {
                execute_actions(rule->then_actions, rule->then_count);
            } else {
                execute_actions(rule->else_actions, rule->else_count);
            }
        }
    }

    s_event_depth--;
}

/* ============================================================================
 * Event Processing
 * ============================================================================ */

static void process_event(rule_event_type_t event_type, uint8_t param, uint16_t time_val)
{
    s_event_depth = 0;

    for (uint8_t i = 0; i < s_rule_count; i++) {
        rule_t *rule = &s_rules[i];

        bool match = false;
        switch (event_type) {
            case EVT_SENSOR:
                match = (rule->event_type == EVT_SENSOR && rule->event_param == param);
                break;
            case EVT_TIMER:
                match = (rule->event_type == EVT_TIMER && rule->event_param == param);
                break;
            case EVT_BOOT:
                match = (rule->event_type == EVT_BOOT);
                break;
            case EVT_VAR_CHANGED:
                match = (rule->event_type == EVT_VAR_CHANGED && rule->event_param == param);
                break;
            case EVT_TIME:
                match = (rule->event_type == EVT_TIME && rule->event_time == time_val);
                break;
        }

        if (match) {
            if (eval_conditions(rule)) {
                execute_actions(rule->then_actions, rule->then_count);
            } else {
                execute_actions(rule->else_actions, rule->else_count);
            }
        }
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t rules_engine_init(void)
{
    memset(s_rules, 0, sizeof(s_rules));
    memset(s_variables, 0, sizeof(s_variables));
    memset(s_timers, 0, sizeof(s_timers));
    memset(s_sensor_values, 0, sizeof(s_sensor_values));
    s_rules_text[0] = '\0';
    s_parse_error[0] = '\0';
    s_rule_count = 0;
    s_boot_time = (uint32_t)time(NULL);
    s_last_time_tick = 0xFFFF;

    // Initialize var config defaults (all persistent, default=0)
    for (int i = 0; i < MAX_RULE_VARIABLES; i++) {
        s_var_persist[i] = true;
        s_var_defaults[i] = 0.0f;
    }

    // Load var config from NVS (persist_mask + defaults)
    uint8_t persist_mask = 0xFF;
    float loaded_defaults[MAX_RULE_VARIABLES];
    memset(loaded_defaults, 0, sizeof(loaded_defaults));
    if (nvs_load_rules_var_config(&persist_mask, loaded_defaults, MAX_RULE_VARIABLES) == ESP_OK) {
        for (int i = 0; i < MAX_RULE_VARIABLES; i++) {
            s_var_persist[i] = (persist_mask >> i) & 0x01;
            s_var_defaults[i] = loaded_defaults[i];
        }
    }

    // Load rules text from NVS
    size_t len = MAX_RULES_TEXT_SIZE;
    if (nvs_load_rules_text(s_rules_text, &len) == ESP_OK && len > 0) {
        ESP_LOGI(TAG, "Loaded rules text from NVS (%d bytes)", (int)len);
        esp_err_t ret = parse_rules(s_rules_text);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Parse error on stored rules: %s", s_parse_error);
        }
    } else {
        ESP_LOGI(TAG, "No rules stored in NVS");
    }

    // Load variables from NVS
    nvs_load_rules_vars(s_variables, MAX_RULE_VARIABLES);

    // Apply defaults for non-persistent variables
    for (int i = 0; i < MAX_RULE_VARIABLES; i++) {
        if (!s_var_persist[i]) {
            s_variables[i] = s_var_defaults[i];
        }
    }

    // Initialize sensor values from current device readings
    uint8_t count = device_manager_get_count();
    for (uint8_t i = 0; i < count && i < MAX_DEVICES; i++) {
        device_config_t dev;
        if (device_manager_get_by_index(i, &dev) == ESP_OK && dev.reading.valid) {
            s_sensor_values[i] = dev.reading.converted_value;
        }
    }

    ESP_LOGI(TAG, "Rules engine initialized with %d rules", s_rule_count);
    return ESP_OK;
}

esp_err_t rules_engine_load_text(const char *text)
{
    if (text == NULL) {
        s_rules_text[0] = '\0';
        s_rule_count = 0;
        return ESP_OK;
    }

    size_t len = strlen(text);
    if (len >= MAX_RULES_TEXT_SIZE) {
        snprintf(s_parse_error, sizeof(s_parse_error),
                 "Rules text too long (%d bytes, max %d)", (int)len, MAX_RULES_TEXT_SIZE - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    // Try parsing before committing
    strncpy(s_rules_text, text, MAX_RULES_TEXT_SIZE - 1);
    s_rules_text[MAX_RULES_TEXT_SIZE - 1] = '\0';

    esp_err_t ret = parse_rules(s_rules_text);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Parse error: %s", s_parse_error);
        // Keep the text but rules won't execute
    }

    return ret;
}

const char* rules_engine_get_text(void)
{
    return s_rules_text;
}

esp_err_t rules_engine_save(void)
{
    esp_err_t ret = nvs_save_rules_text(s_rules_text, strlen(s_rules_text));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save rules text to NVS");
        return ret;
    }

    // For non-persistent variables, save the default value instead of current value
    float vars_to_save[MAX_RULE_VARIABLES];
    for (int i = 0; i < MAX_RULE_VARIABLES; i++) {
        vars_to_save[i] = s_var_persist[i] ? s_variables[i] : s_var_defaults[i];
    }

    ret = nvs_save_rules_vars(vars_to_save, MAX_RULE_VARIABLES);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save rules variables to NVS");
    }

    return ESP_OK;
}

void rules_engine_on_sensor_data(uint8_t device_index, float value)
{
    if (device_index >= MAX_DEVICES) return;
    s_sensor_values[device_index] = value;

    if (s_rule_count > 0) {
        process_event(EVT_SENSOR, device_index, 0);
    }
}

void rules_engine_on_timer(uint8_t timer_id)
{
    if (s_rule_count > 0) {
        ESP_LOGI(TAG, "Timer %d fired", timer_id + 1);
        process_event(EVT_TIMER, timer_id, 0);
    }
}

void rules_engine_on_boot(void)
{
    s_boot_time = (uint32_t)time(NULL);
    if (s_rule_count > 0) {
        ESP_LOGI(TAG, "Boot event");
        process_event(EVT_BOOT, 0, 0);
    }
}

void rules_engine_on_time_tick(uint8_t hour, uint8_t minute)
{
    uint16_t current = hour * 60 + minute;

    if (current == s_last_time_tick) return;  // Already processed this minute
    s_last_time_tick = current;

    if (s_rule_count > 0) {
        process_event(EVT_TIME, 0, current);
    }
}

float rules_engine_get_var(uint8_t index)
{
    if (index >= MAX_RULE_VARIABLES) return 0.0f;
    return s_variables[index];
}

void rules_engine_set_var(uint8_t index, float value)
{
    if (index >= MAX_RULE_VARIABLES) return;
    float old = s_variables[index];
    s_variables[index] = value;
    if (fabsf(old - value) >= 0.001f && s_rule_count > 0) {
        s_event_depth = 0;
        rules_engine_on_var_changed(index);
    }
}

void rules_engine_timer_tick(void)
{
    // Called every 10 seconds by scheduler
    for (uint8_t i = 0; i < MAX_RULE_TIMERS; i++) {
        if (s_timers[i].active) {
            s_timers[i].remaining_seconds -= 10;
            if (s_timers[i].remaining_seconds <= 0) {
                s_timers[i].active = false;
                s_timers[i].remaining_seconds = 0;
                rules_engine_on_timer(i);
            }
        }
    }
}

uint8_t rules_engine_get_rule_count(void)
{
    return s_rule_count;
}

void rules_engine_get_timer_state(uint8_t index, bool *active, int32_t *remaining)
{
    if (index >= MAX_RULE_TIMERS) {
        *active = false;
        *remaining = 0;
        return;
    }
    *active = s_timers[index].active;
    *remaining = s_timers[index].remaining_seconds;
}

const char* rules_engine_get_parse_error(void)
{
    return s_parse_error;
}

void rules_engine_set_var_config(uint8_t index, bool persist, float default_value)
{
    if (index >= MAX_RULE_VARIABLES) return;
    s_var_persist[index] = persist;
    s_var_defaults[index] = default_value;
    ESP_LOGI(TAG, "var%d config: persist=%d default=%.2f", index + 1, persist, default_value);
}

void rules_engine_get_var_config(uint8_t index, bool *persist, float *default_value)
{
    if (index >= MAX_RULE_VARIABLES) {
        if (persist) *persist = true;
        if (default_value) *default_value = 0.0f;
        return;
    }
    if (persist) *persist = s_var_persist[index];
    if (default_value) *default_value = s_var_defaults[index];
}

esp_err_t rules_engine_save_var_config(void)
{
    uint8_t persist_mask = 0;
    for (int i = 0; i < MAX_RULE_VARIABLES; i++) {
        if (s_var_persist[i]) {
            persist_mask |= (1 << i);
        }
    }
    return nvs_save_rules_var_config(persist_mask, s_var_defaults, MAX_RULE_VARIABLES);
}
