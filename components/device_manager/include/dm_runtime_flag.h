#pragma once

#include <stdbool.h>

#include "device_manager.h"
#include "dm_templates.h"

typedef struct {
    dm_flag_trigger_template_t config;
    struct {
        bool valid;
        bool last_state;
    } rules[DM_FLAG_TRIGGER_MAX_RULES];
} dm_flag_trigger_runtime_t;

void dm_flag_trigger_runtime_init(dm_flag_trigger_runtime_t *rt, const dm_flag_trigger_template_t *tpl);
const dm_flag_trigger_rule_t *dm_flag_trigger_runtime_handle(dm_flag_trigger_runtime_t *rt,
                                                             const char *flag_name,
                                                             bool new_state);
