#pragma once

#include "device_manager.h"
#include "dm_templates.h"

typedef struct {
    dm_mqtt_trigger_template_t config;
} dm_mqtt_trigger_runtime_t;

void dm_mqtt_trigger_runtime_init(dm_mqtt_trigger_runtime_t *rt, const dm_mqtt_trigger_template_t *tpl);
const dm_mqtt_trigger_rule_t *dm_mqtt_trigger_runtime_match(dm_mqtt_trigger_runtime_t *rt,
                                                            const char *topic,
                                                            const char *payload);
