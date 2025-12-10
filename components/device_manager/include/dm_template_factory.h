#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "dm_templates.h"
#include "dm_template_registry.h"

typedef struct {
    const char *source_id;
    const char *label;
    const char *const *values;
    size_t value_count;
} dm_uid_slot_param_t;

typedef struct {
    const dm_uid_slot_param_t *slots;
    size_t slot_count;

    const char *success_topic;
    const char *success_payload;
    const char *fail_topic;
    const char *fail_payload;

    const char *success_audio_track;
    const char *fail_audio_track;

    const char *success_signal_topic;
    const char *success_signal_payload;
    const char *fail_signal_topic;
    const char *fail_signal_payload;
} dm_uid_template_params_t;

typedef struct {
    const char *signal_topic;
    const char *signal_payload_on;
    const char *signal_payload_off;
    uint32_t signal_on_ms;

    const char *heartbeat_topic;
    uint32_t required_hold_ms;
    uint32_t heartbeat_timeout_ms;

    const char *hold_track;
    bool hold_track_loop;
    const char *complete_track;
} dm_signal_hold_params_t;

esp_err_t dm_template_factory_build_uid(const dm_uid_template_params_t *params, dm_template_config_t *out);
esp_err_t dm_template_factory_build_signal_hold(const dm_signal_hold_params_t *params, dm_template_config_t *out);
