#include "dm_runtime_mqtt.h"

#include <string.h>

#include "device_manager_utils.h"

void dm_mqtt_trigger_runtime_init(dm_mqtt_trigger_runtime_t *rt, const dm_mqtt_trigger_template_t *tpl)
{
    if (!rt) {
        return;
    }
    if (tpl) {
        memcpy(&rt->config, tpl, sizeof(*tpl));
    } else {
        memset(&rt->config, 0, sizeof(rt->config));
    }
}

static bool payload_matches(const dm_mqtt_trigger_rule_t *rule, const char *payload)
{
    if (!rule) {
        return false;
    }
    if (!rule->payload[0]) {
        return !rule->payload_required;
    }
    if (!rule->payload_required) {
        return true;
    }
    if (!payload) {
        payload = "";
    }
    return strcmp(rule->payload, payload) == 0;
}

const dm_mqtt_trigger_rule_t *dm_mqtt_trigger_runtime_match(dm_mqtt_trigger_runtime_t *rt,
                                                            const char *topic,
                                                            const char *payload)
{
    if (!rt || !topic || !topic[0]) {
        return NULL;
    }
    for (uint8_t i = 0; i < rt->config.rule_count && i < DM_MQTT_TRIGGER_MAX_RULES; ++i) {
        const dm_mqtt_trigger_rule_t *rule = &rt->config.rules[i];
        if (!rule->topic[0] || !rule->scenario[0]) {
            continue;
        }
        if (strcmp(rule->topic, topic) != 0) {
            continue;
        }
        if (!payload_matches(rule, payload)) {
            continue;
        }
        return rule;
    }
    return NULL;
}
