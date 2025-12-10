#pragma once

#include <stddef.h>
#include "dm_templates.h"

typedef enum {
    DM_TEMPLATE_TYPE_UID = 0,
    DM_TEMPLATE_TYPE_SIGNAL_HOLD,
    DM_TEMPLATE_TYPE_MQTT_TRIGGER,
    DM_TEMPLATE_TYPE_FLAG_TRIGGER,
    DM_TEMPLATE_TYPE_COUNT,
} dm_template_type_t;

typedef struct {
    dm_template_type_t type;
    const char *id;
    const char *label;
    const char *description;
} dm_template_descriptor_t;

typedef struct {
    dm_template_type_t type;
    union {
        dm_uid_template_t uid;
        dm_signal_hold_template_t signal;
        dm_mqtt_trigger_template_t mqtt;
        dm_flag_trigger_template_t flag;
    } data;
} dm_template_config_t;

const dm_template_descriptor_t *dm_template_registry_get_all(size_t *count);
const dm_template_descriptor_t *dm_template_find(const char *id);
const char *dm_template_type_to_string(dm_template_type_t type);
bool dm_template_type_from_string(const char *name, dm_template_type_t *out);
