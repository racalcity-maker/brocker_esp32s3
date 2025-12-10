#include "dm_template_registry.h"

#include <string.h>

static const dm_template_descriptor_t s_templates[] = {
    {
        .type = DM_TEMPLATE_TYPE_UID,
        .id = "uid_validator",
        .label = "UID Validator",
        .description = "Validates a set of identifiers reported by multiple sources.",
    },
    {
        .type = DM_TEMPLATE_TYPE_SIGNAL_HOLD,
        .id = "signal_hold",
        .label = "Signal Hold Timer",
        .description = "Accumulates heartbeat duration and triggers actions when the hold completes.",
    },
};

const dm_template_descriptor_t *dm_template_registry_get_all(size_t *count)
{
    if (count) {
        *count = sizeof(s_templates) / sizeof(s_templates[0]);
    }
    return s_templates;
}

const dm_template_descriptor_t *dm_template_find(const char *id)
{
    if (!id || !id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_templates) / sizeof(s_templates[0]); ++i) {
        if (strcasecmp(s_templates[i].id, id) == 0) {
            return &s_templates[i];
        }
    }
    return NULL;
}

const char *dm_template_type_to_string(dm_template_type_t type)
{
    for (size_t i = 0; i < sizeof(s_templates) / sizeof(s_templates[0]); ++i) {
        if (s_templates[i].type == type) {
            return s_templates[i].id;
        }
    }
    return "unknown";
}

bool dm_template_type_from_string(const char *name, dm_template_type_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_templates) / sizeof(s_templates[0]); ++i) {
        if (strcasecmp(s_templates[i].id, name) == 0) {
            *out = s_templates[i].type;
            return true;
        }
    }
    return false;
}
