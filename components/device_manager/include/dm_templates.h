#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dm_limits.h"

typedef enum {
    DEVICE_CONDITION_ALL = 0,
    DEVICE_CONDITION_ANY,
} device_condition_type_t;

// Generic UID validation template ------------------------------------------------

#define DM_UID_TEMPLATE_MAX_SLOTS      8
#define DM_UID_TEMPLATE_MAX_VALUES     8
#define DM_UID_TEMPLATE_VALUE_MAX_LEN  32

typedef struct {
    char source_id[DEVICE_MANAGER_ID_MAX_LEN];
    char label[DEVICE_MANAGER_NAME_MAX_LEN];
    uint8_t value_count;
    char values[DM_UID_TEMPLATE_MAX_VALUES][DM_UID_TEMPLATE_VALUE_MAX_LEN];
} dm_uid_slot_t;

typedef struct {
    dm_uid_slot_t slots[DM_UID_TEMPLATE_MAX_SLOTS];
    uint8_t slot_count;

    char start_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char start_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];

    char broadcast_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char broadcast_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];

    char success_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char success_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char fail_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char fail_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];

    char success_audio_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];
    char fail_audio_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];

    char success_signal_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char success_signal_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char fail_signal_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char fail_signal_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
} dm_uid_template_t;

typedef enum {
    DM_UID_EVENT_NONE = 0,
    DM_UID_EVENT_ACCEPTED,
    DM_UID_EVENT_DUPLICATE,
    DM_UID_EVENT_INVALID,
    DM_UID_EVENT_SUCCESS,
} dm_uid_event_type_t;

typedef struct {
    dm_uid_event_type_t type;
    const dm_uid_slot_t *slot;
} dm_uid_event_t;

typedef struct {
    uint8_t ok_bitmap[(DM_UID_TEMPLATE_MAX_SLOTS + 7) / 8];
    uint8_t ok_count;
    bool failed;
} dm_uid_state_t;

void dm_uid_template_clear(dm_uid_template_t *tpl);
bool dm_uid_template_set_slot(dm_uid_template_t *tpl, uint8_t index, const char *source_id, const char *label);
bool dm_uid_template_add_value(dm_uid_template_t *tpl, uint8_t slot_index, const char *value);

void dm_uid_state_reset(dm_uid_state_t *state);
dm_uid_event_t dm_uid_handle_value(dm_uid_state_t *state,
                                   const dm_uid_template_t *tpl,
                                   const char *source_id,
                                   const char *value);
bool dm_uid_state_is_complete(const dm_uid_state_t *state, const dm_uid_template_t *tpl);

// Signal hold template -----------------------------------------------------------

typedef struct {
    char signal_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char signal_payload_on[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char signal_payload_off[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    uint32_t signal_on_ms;

    char heartbeat_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char reset_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    uint32_t required_hold_ms;
    uint32_t heartbeat_timeout_ms;

    char hold_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];
    bool hold_track_loop;
    char complete_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];
} dm_signal_hold_template_t;

typedef enum {
    DM_SIGNAL_EVENT_NONE = 0,
    DM_SIGNAL_EVENT_START,
    DM_SIGNAL_EVENT_CONTINUE,
    DM_SIGNAL_EVENT_STOP,
    DM_SIGNAL_EVENT_COMPLETED,
} dm_signal_event_type_t;

typedef struct {
    dm_signal_event_type_t type;
    uint32_t accumulated_ms;
} dm_signal_event_t;

typedef struct {
    bool active;
    bool finished;
    uint64_t last_tick_ms;
    uint32_t accumulated_ms;
    bool signal_sent;
} dm_signal_state_t;

void dm_signal_template_clear(dm_signal_hold_template_t *tpl);
void dm_signal_state_reset(dm_signal_state_t *state);
dm_signal_event_t dm_signal_handle_tick(dm_signal_state_t *state,
                                        const dm_signal_hold_template_t *tpl,
                                        uint64_t now_ms);
dm_signal_event_t dm_signal_handle_timeout(dm_signal_state_t *state,
                                           const dm_signal_hold_template_t *tpl);

// MQTT trigger template ---------------------------------------------------------

#define DM_MQTT_TRIGGER_MAX_RULES 8

typedef struct {
    char name[DEVICE_MANAGER_NAME_MAX_LEN];
    char topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char scenario[DEVICE_MANAGER_ID_MAX_LEN];
    bool payload_required;
} dm_mqtt_trigger_rule_t;

typedef struct {
    dm_mqtt_trigger_rule_t rules[DM_MQTT_TRIGGER_MAX_RULES];
    uint8_t rule_count;
} dm_mqtt_trigger_template_t;

void dm_mqtt_trigger_template_clear(dm_mqtt_trigger_template_t *tpl);

// Flag trigger template ---------------------------------------------------------

#define DM_FLAG_TRIGGER_MAX_RULES 8

typedef struct {
    char name[DEVICE_MANAGER_NAME_MAX_LEN];
    char flag[DEVICE_MANAGER_FLAG_NAME_MAX_LEN];
    bool required_state;
    char scenario[DEVICE_MANAGER_ID_MAX_LEN];
} dm_flag_trigger_rule_t;

typedef struct {
    dm_flag_trigger_rule_t rules[DM_FLAG_TRIGGER_MAX_RULES];
    uint8_t rule_count;
} dm_flag_trigger_template_t;

void dm_flag_trigger_template_clear(dm_flag_trigger_template_t *tpl);

// If-condition template --------------------------------------------------------

#define DM_CONDITION_TEMPLATE_MAX_RULES 8

typedef struct {
    char flag[DEVICE_MANAGER_FLAG_NAME_MAX_LEN];
    bool required_state;
} dm_condition_rule_t;

typedef struct {
    device_condition_type_t mode;
    dm_condition_rule_t rules[DM_CONDITION_TEMPLATE_MAX_RULES];
    uint8_t rule_count;
    char true_scenario[DEVICE_MANAGER_ID_MAX_LEN];
    char false_scenario[DEVICE_MANAGER_ID_MAX_LEN];
} dm_condition_template_t;

void dm_condition_template_clear(dm_condition_template_t *tpl);

// Interval task template ------------------------------------------------------

typedef struct {
    uint32_t interval_ms;
    char scenario[DEVICE_MANAGER_ID_MAX_LEN];
} dm_interval_task_template_t;

void dm_interval_task_template_clear(dm_interval_task_template_t *tpl);

// Sequence lock template ------------------------------------------------------

#define DM_SEQUENCE_TEMPLATE_MAX_STEPS 8

typedef struct {
    char topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    bool payload_required;
    char hint_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char hint_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char hint_audio_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];
} dm_sequence_step_t;

typedef struct {
    dm_sequence_step_t steps[DM_SEQUENCE_TEMPLATE_MAX_STEPS];
    uint8_t step_count;
    uint32_t timeout_ms;
    bool reset_on_error;

    char success_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char success_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char success_audio_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];
    char success_scenario[DEVICE_MANAGER_ID_MAX_LEN];

    char fail_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char fail_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char fail_audio_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];
    char fail_scenario[DEVICE_MANAGER_ID_MAX_LEN];
} dm_sequence_template_t;

void dm_sequence_template_clear(dm_sequence_template_t *tpl);
