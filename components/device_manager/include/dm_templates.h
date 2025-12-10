#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dm_limits.h"

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
