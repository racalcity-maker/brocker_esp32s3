#pragma once

#include "device_manager.h"
#include "dm_templates.h"

typedef struct {
    dm_signal_hold_template_t config;
    dm_signal_state_t state;
} dm_signal_runtime_t;

typedef struct {
    dm_signal_event_type_t event;

    bool audio_play;
    bool audio_pause;
    char audio_track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];

    bool signal_on;
    bool signal_off;
    uint32_t signal_on_ms;
    char signal_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char signal_payload_on[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char signal_payload_off[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
} dm_signal_action_t;

void dm_signal_runtime_init(dm_signal_runtime_t *rt, const dm_signal_hold_template_t *tpl);
void dm_signal_runtime_set_template(dm_signal_runtime_t *rt, const dm_signal_hold_template_t *tpl);
dm_signal_action_t dm_signal_runtime_handle_tick(dm_signal_runtime_t *rt, uint64_t now_ms);
