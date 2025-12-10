#include "dm_runtime_flag.h"

#include <string.h>
#include <strings.h>

#include "device_manager_utils.h"

void dm_flag_trigger_runtime_init(dm_flag_trigger_runtime_t *rt, const dm_flag_trigger_template_t *tpl)
{
    if (!rt) {
        return;
    }
    if (tpl) {
        memcpy(&rt->config, tpl, sizeof(*tpl));
    } else {
        memset(&rt->config, 0, sizeof(rt->config));
    }
    memset(rt->rules, 0, sizeof(rt->rules));
}

const dm_flag_trigger_rule_t *dm_flag_trigger_runtime_handle(dm_flag_trigger_runtime_t *rt,
                                                             const char *flag_name,
                                                             bool new_state)
{
    if (!rt || !flag_name || !flag_name[0]) {
        return NULL;
    }
    for (uint8_t i = 0; i < rt->config.rule_count && i < DM_FLAG_TRIGGER_MAX_RULES; ++i) {
        const dm_flag_trigger_rule_t *rule = &rt->config.rules[i];
        if (!rule->flag[0] || !rule->scenario[0]) {
            continue;
        }
        if (strcasecmp(rule->flag, flag_name) != 0) {
            continue;
        }
        bool changed = (!rt->rules[i].valid) || (rt->rules[i].last_state != new_state);
        rt->rules[i].valid = true;
        rt->rules[i].last_state = new_state;
        if (new_state == rule->required_state && changed) {
            return rule;
        }
    }
    return NULL;
}
