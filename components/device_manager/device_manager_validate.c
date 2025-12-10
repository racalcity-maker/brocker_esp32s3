#include "device_manager_internal.h"

#include <strings.h>

typedef struct {
    device_tab_type_t type;
    const char *name;
} tab_type_map_t;

static const tab_type_map_t k_tab_types[] = {
    {DEVICE_TAB_AUDIO, "audio"},
    {DEVICE_TAB_CUSTOM, "custom"},
};

typedef struct {
    device_action_type_t type;
    const char *name;
} action_type_map_t;

static const action_type_map_t k_action_types[] = {
    {DEVICE_ACTION_NOP, "nop"},
    {DEVICE_ACTION_MQTT_PUBLISH, "mqtt_publish"},
    {DEVICE_ACTION_AUDIO_PLAY, "audio_play"},
    {DEVICE_ACTION_AUDIO_STOP, "audio_stop"},
    {DEVICE_ACTION_SET_FLAG, "set_flag"},
    {DEVICE_ACTION_WAIT_FLAGS, "wait_flags"},
    {DEVICE_ACTION_LOOP, "loop"},
    {DEVICE_ACTION_DELAY, "delay"},
    {DEVICE_ACTION_EVENT_BUS, "event"},
};

const char *dm_condition_to_string(device_condition_type_t cond)
{
    switch (cond) {
    case DEVICE_CONDITION_ALL:
        return "all";
    case DEVICE_CONDITION_ANY:
        return "any";
    default:
        return "all";
    }
}

bool dm_condition_from_string(const char *name, device_condition_type_t *out)
{
    if (!name || !out) {
        return false;
    }
    if (strcasecmp(name, "all") == 0) {
        *out = DEVICE_CONDITION_ALL;
        return true;
    }
    if (strcasecmp(name, "any") == 0) {
        *out = DEVICE_CONDITION_ANY;
        return true;
    }
    return false;
}

const char *dm_tab_type_to_string(device_tab_type_t type)
{
    for (size_t i = 0; i < sizeof(k_tab_types) / sizeof(k_tab_types[0]); ++i) {
        if (k_tab_types[i].type == type) {
            return k_tab_types[i].name;
        }
    }
    return "custom";
}

bool dm_tab_type_from_string(const char *name, device_tab_type_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_tab_types) / sizeof(k_tab_types[0]); ++i) {
        if (strcasecmp(k_tab_types[i].name, name) == 0) {
            *out = k_tab_types[i].type;
            return true;
        }
    }
    return false;
}

const char *dm_action_type_to_string(device_action_type_t type)
{
    for (size_t i = 0; i < sizeof(k_action_types) / sizeof(k_action_types[0]); ++i) {
        if (k_action_types[i].type == type) {
            return k_action_types[i].name;
        }
    }
    return "nop";
}

bool dm_action_type_from_string(const char *name, device_action_type_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_action_types) / sizeof(k_action_types[0]); ++i) {
        if (strcasecmp(k_action_types[i].name, name) == 0) {
            *out = k_action_types[i].type;
            return true;
        }
    }
    return false;
}
