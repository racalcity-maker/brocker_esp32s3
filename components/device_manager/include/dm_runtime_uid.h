#pragma once

#include "device_manager.h"
#include "dm_templates.h"

typedef struct {
    dm_uid_template_t config;
    dm_uid_state_t state;
    struct {
        bool has_value;
        char value[DM_UID_TEMPLATE_VALUE_MAX_LEN];
    } slots[DM_UID_TEMPLATE_MAX_SLOTS];
} dm_uid_runtime_t;

typedef struct {
    dm_uid_event_type_t event;

    bool publish_channel;
    char channel_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char channel_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];

    bool publish_signal;
    char signal_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char signal_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];

    bool audio_play;
    char audio_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];
} dm_uid_action_t;

void dm_uid_runtime_init(dm_uid_runtime_t *rt, const dm_uid_template_t *tpl);
void dm_uid_runtime_set_template(dm_uid_runtime_t *rt, const dm_uid_template_t *tpl);
dm_uid_action_t dm_uid_runtime_handle_value(dm_uid_runtime_t *rt,
                                            const char *source_id,
                                            const char *value);
