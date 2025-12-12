#include "device_manager_internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"

#include "dm_profiles.h"
#include "dm_storage.h"
#include "device_manager_utils.h"
#include "dm_template_runtime.h"

// Helper: add string field if value is non-empty.
static void template_to_json_string(cJSON *obj, const char *key, const char *value)
{
    if (obj && key && value && value[0]) {
        cJSON_AddStringToObject(obj, key, value);
    }
}

// Serialize scenario step into JSON representation.
static cJSON *step_to_json(const device_action_step_t *step)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "type", dm_action_type_to_string(step->type));
    if (step->delay_ms > 0) {
        cJSON_AddNumberToObject(obj, "delay_ms", (double)step->delay_ms);
    }
    switch (step->type) {
    case DEVICE_ACTION_MQTT_PUBLISH:
        cJSON_AddStringToObject(obj, "topic", step->data.mqtt.topic);
        cJSON_AddStringToObject(obj, "payload", step->data.mqtt.payload);
        cJSON_AddNumberToObject(obj, "qos", step->data.mqtt.qos);
        cJSON_AddBoolToObject(obj, "retain", step->data.mqtt.retain);
        break;
    case DEVICE_ACTION_AUDIO_PLAY:
        cJSON_AddStringToObject(obj, "track", step->data.audio.track);
        cJSON_AddBoolToObject(obj, "blocking", step->data.audio.blocking);
        break;
    case DEVICE_ACTION_SET_FLAG:
        cJSON_AddStringToObject(obj, "flag", step->data.flag.flag);
        cJSON_AddBoolToObject(obj, "value", step->data.flag.value);
        break;
    case DEVICE_ACTION_WAIT_FLAGS: {
        cJSON *wait = cJSON_CreateObject();
        if (!wait) {
            cJSON_Delete(obj);
            return NULL;
        }
        cJSON_AddItemToObject(obj, "wait", wait);
        cJSON_AddStringToObject(wait, "mode", dm_condition_to_string(step->data.wait_flags.mode));
        if (step->data.wait_flags.timeout_ms > 0) {
            cJSON_AddNumberToObject(wait, "timeout_ms", (double)step->data.wait_flags.timeout_ms);
        }
        cJSON *reqs = cJSON_AddArrayToObject(wait, "requirements");
        if (!reqs) {
            cJSON_Delete(obj);
            return NULL;
        }
        for (uint8_t i = 0; i < step->data.wait_flags.requirement_count; ++i) {
            cJSON *req = cJSON_CreateObject();
            if (!req) {
                cJSON_Delete(obj);
                return NULL;
            }
            cJSON_AddItemToArray(reqs, req);
            cJSON_AddStringToObject(req, "flag", step->data.wait_flags.requirements[i].flag);
            cJSON_AddBoolToObject(req, "state", step->data.wait_flags.requirements[i].required_state);
        }
        break;
    }
    case DEVICE_ACTION_LOOP: {
        cJSON *loop = cJSON_CreateObject();
        if (!loop) {
            cJSON_Delete(obj);
            return NULL;
        }
        cJSON_AddItemToObject(obj, "loop", loop);
        cJSON_AddNumberToObject(loop, "target_step", step->data.loop.target_step);
        cJSON_AddNumberToObject(loop, "max_iterations", step->data.loop.max_iterations);
        break;
    }
    case DEVICE_ACTION_EVENT_BUS:
        cJSON_AddStringToObject(obj, "event", step->data.event.event);
        if (step->data.event.topic[0]) {
            cJSON_AddStringToObject(obj, "topic", step->data.event.topic);
        }
        if (step->data.event.payload[0]) {
            cJSON_AddStringToObject(obj, "payload", step->data.event.payload);
        }
        break;
    case DEVICE_ACTION_AUDIO_STOP:
    case DEVICE_ACTION_DELAY:
    case DEVICE_ACTION_NOP:
    default:
        break;
    }
    return obj;
}

// Convert UID template runtime state into JSON (including last-read values).
static cJSON *uid_template_to_json(const device_descriptor_t *dev)
{
    if (!dev) {
        return NULL;
    }
    const dm_uid_template_t *tpl = &dev->template_config.data.uid;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *slots = cJSON_AddArrayToObject(root, "slots");
    if (!slots) {
        cJSON_Delete(root);
        return NULL;
    }
    dm_uid_runtime_snapshot_t snapshot;
    bool have_snapshot = (dm_template_runtime_get_uid_snapshot(dev->id, &snapshot) == ESP_OK);
    for (uint8_t i = 0; i < tpl->slot_count && i < DM_UID_TEMPLATE_MAX_SLOTS; ++i) {
        const dm_uid_slot_t *slot = &tpl->slots[i];
        if (!slot->source_id[0]) {
            continue;
        }
        cJSON *slot_obj = cJSON_CreateObject();
        if (!slot_obj) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(slots, slot_obj);
        cJSON_AddStringToObject(slot_obj, "source_id", slot->source_id);
        if (slot->label[0]) {
            cJSON_AddStringToObject(slot_obj, "label", slot->label);
        }
        cJSON *values = cJSON_AddArrayToObject(slot_obj, "values");
        if (!values) {
            cJSON_Delete(root);
            return NULL;
        }
        for (uint8_t v = 0; v < slot->value_count && v < DM_UID_TEMPLATE_MAX_VALUES; ++v) {
            if (slot->values[v][0]) {
                cJSON_AddItemToArray(values, cJSON_CreateString(slot->values[v]));
            }
        }
        if (have_snapshot && i < snapshot.slot_count && snapshot.slots[i].has_value) {
            cJSON_AddStringToObject(slot_obj, "last_value", snapshot.slots[i].last_value);
        }
    }
    template_to_json_string(root, "start_topic", tpl->start_topic);
    template_to_json_string(root, "start_payload", tpl->start_payload);
    template_to_json_string(root, "broadcast_topic", tpl->broadcast_topic);
    template_to_json_string(root, "broadcast_payload", tpl->broadcast_payload);
    template_to_json_string(root, "success_topic", tpl->success_topic);
    template_to_json_string(root, "success_payload", tpl->success_payload);
    template_to_json_string(root, "fail_topic", tpl->fail_topic);
    template_to_json_string(root, "fail_payload", tpl->fail_payload);
    template_to_json_string(root, "success_audio_track", tpl->success_audio_track);
    template_to_json_string(root, "fail_audio_track", tpl->fail_audio_track);
    template_to_json_string(root, "success_signal_topic", tpl->success_signal_topic);
    template_to_json_string(root, "success_signal_payload", tpl->success_signal_payload);
    template_to_json_string(root, "fail_signal_topic", tpl->fail_signal_topic);
    template_to_json_string(root, "fail_signal_payload", tpl->fail_signal_payload);
    return root;
}

// Serialize signal-hold template.
static cJSON *signal_template_to_json(const dm_signal_hold_template_t *tpl)
{
    if (!tpl) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    template_to_json_string(root, "signal_topic", tpl->signal_topic);
    template_to_json_string(root, "signal_payload_on", tpl->signal_payload_on);
    template_to_json_string(root, "signal_payload_off", tpl->signal_payload_off);
    cJSON_AddNumberToObject(root, "signal_on_ms", (double)tpl->signal_on_ms);
    template_to_json_string(root, "heartbeat_topic", tpl->heartbeat_topic);
    template_to_json_string(root, "reset_topic", tpl->reset_topic);
    cJSON_AddNumberToObject(root, "required_hold_ms", (double)tpl->required_hold_ms);
    cJSON_AddNumberToObject(root, "heartbeat_timeout_ms", (double)tpl->heartbeat_timeout_ms);
    template_to_json_string(root, "hold_track", tpl->hold_track);
    cJSON_AddBoolToObject(root, "hold_track_loop", tpl->hold_track_loop);
    template_to_json_string(root, "complete_track", tpl->complete_track);
    return root;
}

static cJSON *mqtt_template_to_json(const dm_mqtt_trigger_template_t *tpl)
{
    if (!tpl) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *rules = cJSON_AddArrayToObject(root, "rules");
    if (!rules) {
        cJSON_Delete(root);
        return NULL;
    }
    for (uint8_t i = 0; i < tpl->rule_count && i < DM_MQTT_TRIGGER_MAX_RULES; ++i) {
        const dm_mqtt_trigger_rule_t *rule = &tpl->rules[i];
        if (!rule->topic[0] || !rule->scenario[0]) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(rules, obj);
        if (rule->name[0]) {
            cJSON_AddStringToObject(obj, "name", rule->name);
        }
        cJSON_AddStringToObject(obj, "topic", rule->topic);
        if (rule->payload[0]) {
            cJSON_AddStringToObject(obj, "payload", rule->payload);
        }
        if (rule->payload[0] || rule->payload_required) {
            cJSON_AddBoolToObject(obj, "payload_required", rule->payload_required);
        }
        cJSON_AddStringToObject(obj, "scenario", rule->scenario);
    }
    return root;
}

static cJSON *flag_template_to_json(const dm_flag_trigger_template_t *tpl)
{
    if (!tpl) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *rules = cJSON_AddArrayToObject(root, "rules");
    if (!rules) {
        cJSON_Delete(root);
        return NULL;
    }
    for (uint8_t i = 0; i < tpl->rule_count && i < DM_FLAG_TRIGGER_MAX_RULES; ++i) {
        const dm_flag_trigger_rule_t *rule = &tpl->rules[i];
        if (!rule->flag[0] || !rule->scenario[0]) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(rules, obj);
        if (rule->name[0]) {
            cJSON_AddStringToObject(obj, "name", rule->name);
        }
        cJSON_AddStringToObject(obj, "flag", rule->flag);
        cJSON_AddBoolToObject(obj, "state", rule->required_state);
        cJSON_AddStringToObject(obj, "scenario", rule->scenario);
    }
    return root;
}

static cJSON *condition_template_to_json(const dm_condition_template_t *tpl)
{
    if (!tpl) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "mode", dm_condition_to_string(tpl->mode));
    if (tpl->true_scenario[0]) {
        cJSON_AddStringToObject(root, "true_scenario", tpl->true_scenario);
    }
    if (tpl->false_scenario[0]) {
        cJSON_AddStringToObject(root, "false_scenario", tpl->false_scenario);
    }
    cJSON *rules = cJSON_AddArrayToObject(root, "rules");
    if (!rules) {
        cJSON_Delete(root);
        return NULL;
    }
    for (uint8_t i = 0; i < tpl->rule_count && i < DM_CONDITION_TEMPLATE_MAX_RULES; ++i) {
        const dm_condition_rule_t *rule = &tpl->rules[i];
        if (!rule->flag[0]) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(rules, obj);
        cJSON_AddStringToObject(obj, "flag", rule->flag);
        cJSON_AddBoolToObject(obj, "state", rule->required_state);
    }
    return root;
}

static cJSON *interval_template_to_json(const dm_interval_task_template_t *tpl)
{
    if (!tpl) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "interval_ms", (double)tpl->interval_ms);
    template_to_json_string(root, "scenario", tpl->scenario);
    return root;
}

static cJSON *sequence_template_to_json(const dm_sequence_template_t *tpl)
{
    if (!tpl) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *steps = cJSON_AddArrayToObject(root, "steps");
    if (!steps) {
        cJSON_Delete(root);
        return NULL;
    }
    for (uint8_t i = 0; i < tpl->step_count && i < DM_SEQUENCE_TEMPLATE_MAX_STEPS; ++i) {
        const dm_sequence_step_t *step = &tpl->steps[i];
        if (!step->topic[0]) {
            continue;
        }
        cJSON *step_obj = cJSON_CreateObject();
        if (!step_obj) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(steps, step_obj);
        cJSON_AddStringToObject(step_obj, "topic", step->topic);
        if (step->payload[0]) {
            cJSON_AddStringToObject(step_obj, "payload", step->payload);
        }
        cJSON_AddBoolToObject(step_obj, "payload_required", step->payload_required);
        if (step->hint_topic[0]) {
            cJSON_AddStringToObject(step_obj, "hint_topic", step->hint_topic);
        }
        if (step->hint_payload[0]) {
            cJSON_AddStringToObject(step_obj, "hint_payload", step->hint_payload);
        }
        if (step->hint_audio_track[0]) {
            cJSON_AddStringToObject(step_obj, "hint_audio_track", step->hint_audio_track);
        }
    }
    if (tpl->timeout_ms > 0) {
        cJSON_AddNumberToObject(root, "timeout_ms", (double)tpl->timeout_ms);
    }
    cJSON_AddBoolToObject(root, "reset_on_error", tpl->reset_on_error);
    template_to_json_string(root, "success_topic", tpl->success_topic);
    template_to_json_string(root, "success_payload", tpl->success_payload);
    template_to_json_string(root, "success_audio_track", tpl->success_audio_track);
    template_to_json_string(root, "success_scenario", tpl->success_scenario);
    template_to_json_string(root, "fail_topic", tpl->fail_topic);
    template_to_json_string(root, "fail_payload", tpl->fail_payload);
    template_to_json_string(root, "fail_audio_track", tpl->fail_audio_track);
    template_to_json_string(root, "fail_scenario", tpl->fail_scenario);
    return root;
}

// Attach template-specific structure into device JSON.
static cJSON *template_to_json(const device_descriptor_t *dev)
{
    if (!dev || !dev->template_assigned) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    const char *type = dm_template_type_to_string(dev->template_config.type);
    cJSON_AddStringToObject(root, "type", type);
    cJSON *data = NULL;
    switch (dev->template_config.type) {
    case DM_TEMPLATE_TYPE_UID:
        data = uid_template_to_json(dev);
        if (data) {
            cJSON_AddItemToObject(root, "uid", data);
        }
        break;
    case DM_TEMPLATE_TYPE_SIGNAL_HOLD:
        data = signal_template_to_json(&dev->template_config.data.signal);
        if (data) {
            cJSON_AddItemToObject(root, "signal", data);
        }
        break;
    case DM_TEMPLATE_TYPE_MQTT_TRIGGER:
        data = mqtt_template_to_json(&dev->template_config.data.mqtt);
        if (data) {
            cJSON_AddItemToObject(root, "mqtt", data);
        }
        break;
    case DM_TEMPLATE_TYPE_FLAG_TRIGGER:
        data = flag_template_to_json(&dev->template_config.data.flag);
        if (data) {
            cJSON_AddItemToObject(root, "flag", data);
        }
        break;
    case DM_TEMPLATE_TYPE_IF_CONDITION:
        data = condition_template_to_json(&dev->template_config.data.condition);
        if (data) {
            cJSON_AddItemToObject(root, "condition", data);
        }
        break;
    case DM_TEMPLATE_TYPE_INTERVAL_TASK:
        data = interval_template_to_json(&dev->template_config.data.interval);
        if (data) {
            cJSON_AddItemToObject(root, "interval", data);
        }
        break;
    case DM_TEMPLATE_TYPE_SEQUENCE_LOCK:
        data = sequence_template_to_json(&dev->template_config.data.sequence);
        if (data) {
            cJSON_AddItemToObject(root, "sequence", data);
        }
        break;
    default:
        break;
    }
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

// Build JSON snapshot of entire configuration; caller must free `*out_json`.
esp_err_t dm_storage_internal_export(const device_manager_config_t *cfg, char **out_json, size_t *out_len)
{
    if (!cfg || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    if (out_len) {
        *out_len = 0;
    }
    esp_err_t err = ESP_OK;
    dm_cjson_install_hooks();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        dm_cjson_reset_hooks();
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema", cfg->schema_version);
    cJSON_AddNumberToObject(root, "generation", cfg->generation);
    const char *active_profile = cfg->active_profile[0] ? cfg->active_profile : DM_DEFAULT_PROFILE_ID;
    cJSON_AddStringToObject(root, "active_profile", active_profile);
    cJSON *profiles = cJSON_AddArrayToObject(root, "profiles");
    if (!profiles) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    for (uint8_t i = 0; i < cfg->profile_count && i < DEVICE_MANAGER_MAX_PROFILES; ++i) {
        const device_manager_profile_t *profile = &cfg->profiles[i];
        if (!profile->id[0]) {
            continue;
        }
        cJSON *p = cJSON_CreateObject();
        if (!p) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddItemToArray(profiles, p);
        cJSON_AddStringToObject(p, "id", profile->id);
        cJSON_AddStringToObject(p, "name", profile->name[0] ? profile->name : profile->id);
        cJSON_AddNumberToObject(p, "device_count", profile->device_count);
        if (strcasecmp(profile->id, active_profile) == 0) {
            cJSON_AddBoolToObject(p, "active", true);
        }
    }

    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    if (!devices) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    uint8_t device_cap = cfg->device_capacity ? cfg->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
    for (uint8_t i = 0; i < cfg->device_count && i < device_cap; ++i) {
        feed_wdt();
        const device_descriptor_t *dev = &cfg->devices[i];
        cJSON *d = cJSON_CreateObject();
        if (!d) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddItemToArray(devices, d);
        const char *display_name = dev->display_name[0] ? dev->display_name : dev->id;
        cJSON_AddStringToObject(d, "id", dev->id);
        cJSON_AddStringToObject(d, "name", display_name);
        cJSON_AddStringToObject(d, "display_name", display_name);

        cJSON *topics = cJSON_AddArrayToObject(d, "topics");
        if (!topics) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        for (uint8_t tp = 0; tp < dev->topic_count && tp < DEVICE_MANAGER_MAX_TOPICS_PER_DEVICE; ++tp) {
            const device_topic_binding_t *binding = &dev->topics[tp];
            cJSON *topic_obj = cJSON_CreateObject();
            if (!topic_obj) {
                err = ESP_ERR_NO_MEM;
                goto cleanup;
            }
            cJSON_AddItemToArray(topics, topic_obj);
            cJSON_AddStringToObject(topic_obj, "name", binding->name);
            cJSON_AddStringToObject(topic_obj, "topic", binding->topic);
            feed_wdt();
        }

        cJSON *scenarios = cJSON_AddArrayToObject(d, "scenarios");
        if (!scenarios) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        for (uint8_t s = 0; s < dev->scenario_count && s < DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE; ++s) {
            const device_scenario_t *sc = &dev->scenarios[s];
            cJSON *sc_obj = cJSON_CreateObject();
            if (!sc_obj) {
                err = ESP_ERR_NO_MEM;
                goto cleanup;
            }
            cJSON_AddItemToArray(scenarios, sc_obj);
            cJSON_AddStringToObject(sc_obj, "id", sc->id);
            cJSON_AddStringToObject(sc_obj, "name", sc->name);
            cJSON_AddBoolToObject(sc_obj, "button_enabled", sc->button_enabled);
            cJSON_AddStringToObject(sc_obj, "button_label", sc->button_label);
            cJSON *steps = cJSON_AddArrayToObject(sc_obj, "steps");
            if (!steps) {
                err = ESP_ERR_NO_MEM;
                goto cleanup;
            }
            for (uint8_t st = 0; st < sc->step_count && st < DEVICE_MANAGER_MAX_STEPS_PER_SCENARIO; ++st) {
                cJSON *step_obj = step_to_json(&sc->steps[st]);
                if (!step_obj) {
                    err = ESP_ERR_NO_MEM;
                    goto cleanup;
                }
                cJSON_AddItemToArray(steps, step_obj);
                feed_wdt();
            }
        }

        if (dev->template_assigned) {
            cJSON *tpl_obj = template_to_json(dev);
            if (!tpl_obj) {
                err = ESP_ERR_NO_MEM;
                goto cleanup;
            }
            cJSON_AddItemToObject(d, "template", tpl_obj);
        }
    }

    {
        char *printed = cJSON_PrintUnformatted(root);
        if (!printed) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        size_t len = strlen(printed);
        char *buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            free(printed);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        memcpy(buf, printed, len + 1);
        free(printed);
        *out_json = buf;
        if (out_len) {
            *out_len = len;
        }
    }

cleanup:
    if (root) {
        cJSON_Delete(root);
    }
    dm_cjson_reset_hooks();
    return err;
}
