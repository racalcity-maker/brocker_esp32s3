#include "device_manager_internal.h"

#include <string.h>

#include "esp_log.h"

#include "dm_profiles.h"
#include "dm_storage.h"
#include "device_manager_utils.h"

static const char *TAG = "device_manager";

// Utility: clamp JSON numeric field to uint32_t range or default.
static uint32_t json_number_to_u32(const cJSON *item, uint32_t default_val)
{
    if (!item || !cJSON_IsNumber(item) || item->valuedouble < 0) {
        return default_val;
    }
    double v = item->valuedouble;
    if (v > (double)UINT32_MAX) {
        v = (double)UINT32_MAX;
    }
    return (uint32_t)v;
}

// Utility: clamp JSON numeric field to uint16_t range or default.
static uint16_t json_number_to_u16(const cJSON *item, uint16_t default_val)
{
    if (!item || !cJSON_IsNumber(item) || item->valuedouble < 0) {
        return default_val;
    }
    double v = item->valuedouble;
    if (v > (double)UINT16_MAX) {
        v = (double)UINT16_MAX;
    }
    return (uint16_t)v;
}

// Return boolean value if present, otherwise fall back to default.
static bool json_get_bool_default(const cJSON *item, bool default_val)
{
    if (!item) {
        return default_val;
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_val;
}

// Parse UID validator configuration from JSON into struct.
static bool uid_template_from_json(dm_uid_template_t *tpl, const cJSON *obj)
{
    if (!tpl || !obj) {
        return false;
    }
    dm_uid_template_clear(tpl);
    const cJSON *slots = cJSON_GetObjectItem(obj, "slots");
    if (!cJSON_IsArray(slots)) {
        return false;
    }
    uint8_t slot_index = 0;
    const cJSON *slot_obj = NULL;
    cJSON_ArrayForEach(slot_obj, slots) {
        if (slot_index >= DM_UID_TEMPLATE_MAX_SLOTS) {
            break;
        }
        const cJSON *source_id = cJSON_GetObjectItem(slot_obj, "source_id");
        if (!cJSON_IsString(source_id) || !source_id->valuestring[0]) {
            continue;
        }
        const cJSON *label = cJSON_GetObjectItem(slot_obj, "label");
        const char *label_str = (cJSON_IsString(label) && label->valuestring) ? label->valuestring : "";
        if (!dm_uid_template_set_slot(tpl, slot_index, source_id->valuestring, label_str)) {
            return false;
        }
        const cJSON *values = cJSON_GetObjectItem(slot_obj, "values");
        if (cJSON_IsArray(values)) {
            const cJSON *val = NULL;
            cJSON_ArrayForEach(val, values) {
                if (!cJSON_IsString(val) || !val->valuestring[0]) {
                    continue;
                }
                if (!dm_uid_template_add_value(tpl, slot_index, val->valuestring)) {
                    return false;
                }
            }
        }
        slot_index++;
    }
    tpl->slot_count = slot_index;
    const cJSON *start_topic = cJSON_GetObjectItem(obj, "start_topic");
    if (cJSON_IsString(start_topic) && start_topic->valuestring) {
        dm_str_copy(tpl->start_topic, sizeof(tpl->start_topic), start_topic->valuestring);
    }
    const cJSON *start_payload = cJSON_GetObjectItem(obj, "start_payload");
    if (cJSON_IsString(start_payload) && start_payload->valuestring) {
        dm_str_copy(tpl->start_payload, sizeof(tpl->start_payload), start_payload->valuestring);
    }
    const cJSON *broadcast_topic = cJSON_GetObjectItem(obj, "broadcast_topic");
    if (cJSON_IsString(broadcast_topic) && broadcast_topic->valuestring) {
        dm_str_copy(tpl->broadcast_topic, sizeof(tpl->broadcast_topic), broadcast_topic->valuestring);
    }
    const cJSON *broadcast_payload = cJSON_GetObjectItem(obj, "broadcast_payload");
    if (cJSON_IsString(broadcast_payload) && broadcast_payload->valuestring) {
        dm_str_copy(tpl->broadcast_payload, sizeof(tpl->broadcast_payload), broadcast_payload->valuestring);
    }
    const cJSON *success_topic = cJSON_GetObjectItem(obj, "success_topic");
    if (cJSON_IsString(success_topic) && success_topic->valuestring) {
        dm_str_copy(tpl->success_topic, sizeof(tpl->success_topic), success_topic->valuestring);
    }
    const cJSON *success_payload = cJSON_GetObjectItem(obj, "success_payload");
    if (cJSON_IsString(success_payload) && success_payload->valuestring) {
        dm_str_copy(tpl->success_payload, sizeof(tpl->success_payload), success_payload->valuestring);
    }
    const cJSON *fail_topic = cJSON_GetObjectItem(obj, "fail_topic");
    if (cJSON_IsString(fail_topic) && fail_topic->valuestring) {
        dm_str_copy(tpl->fail_topic, sizeof(tpl->fail_topic), fail_topic->valuestring);
    }
    const cJSON *fail_payload = cJSON_GetObjectItem(obj, "fail_payload");
    if (cJSON_IsString(fail_payload) && fail_payload->valuestring) {
        dm_str_copy(tpl->fail_payload, sizeof(tpl->fail_payload), fail_payload->valuestring);
    }
    const cJSON *success_audio = cJSON_GetObjectItem(obj, "success_audio_track");
    if (cJSON_IsString(success_audio) && success_audio->valuestring) {
        dm_str_copy(tpl->success_audio_track, sizeof(tpl->success_audio_track), success_audio->valuestring);
    }
    const cJSON *fail_audio = cJSON_GetObjectItem(obj, "fail_audio_track");
    if (cJSON_IsString(fail_audio) && fail_audio->valuestring) {
        dm_str_copy(tpl->fail_audio_track, sizeof(tpl->fail_audio_track), fail_audio->valuestring);
    }
    const cJSON *success_signal_topic = cJSON_GetObjectItem(obj, "success_signal_topic");
    if (cJSON_IsString(success_signal_topic) && success_signal_topic->valuestring) {
        dm_str_copy(tpl->success_signal_topic, sizeof(tpl->success_signal_topic), success_signal_topic->valuestring);
    }
    const cJSON *success_signal_payload = cJSON_GetObjectItem(obj, "success_signal_payload");
    if (cJSON_IsString(success_signal_payload) && success_signal_payload->valuestring) {
        dm_str_copy(tpl->success_signal_payload, sizeof(tpl->success_signal_payload), success_signal_payload->valuestring);
    }
    const cJSON *fail_signal_topic = cJSON_GetObjectItem(obj, "fail_signal_topic");
    if (cJSON_IsString(fail_signal_topic) && fail_signal_topic->valuestring) {
        dm_str_copy(tpl->fail_signal_topic, sizeof(tpl->fail_signal_topic), fail_signal_topic->valuestring);
    }
    const cJSON *fail_signal_payload = cJSON_GetObjectItem(obj, "fail_signal_payload");
    if (cJSON_IsString(fail_signal_payload) && fail_signal_payload->valuestring) {
        dm_str_copy(tpl->fail_signal_payload, sizeof(tpl->fail_signal_payload), fail_signal_payload->valuestring);
    }
    return tpl->slot_count > 0;
}

// Parse laser/hold template from JSON object.
static bool signal_template_from_json(dm_signal_hold_template_t *tpl, const cJSON *obj)
{
    if (!tpl || !obj) {
        return false;
    }
    dm_signal_template_clear(tpl);
    const cJSON *signal_topic = cJSON_GetObjectItem(obj, "signal_topic");
    if (cJSON_IsString(signal_topic) && signal_topic->valuestring) {
        dm_str_copy(tpl->signal_topic, sizeof(tpl->signal_topic), signal_topic->valuestring);
    }
    const cJSON *signal_on = cJSON_GetObjectItem(obj, "signal_payload_on");
    if (cJSON_IsString(signal_on) && signal_on->valuestring) {
        dm_str_copy(tpl->signal_payload_on, sizeof(tpl->signal_payload_on), signal_on->valuestring);
    }
    const cJSON *signal_off = cJSON_GetObjectItem(obj, "signal_payload_off");
    if (cJSON_IsString(signal_off) && signal_off->valuestring) {
        dm_str_copy(tpl->signal_payload_off, sizeof(tpl->signal_payload_off), signal_off->valuestring);
    }
    tpl->signal_on_ms = json_number_to_u32(cJSON_GetObjectItem(obj, "signal_on_ms"), tpl->signal_on_ms);
    const cJSON *heartbeat = cJSON_GetObjectItem(obj, "heartbeat_topic");
    if (cJSON_IsString(heartbeat) && heartbeat->valuestring) {
        dm_str_copy(tpl->heartbeat_topic, sizeof(tpl->heartbeat_topic), heartbeat->valuestring);
    }
    const cJSON *reset = cJSON_GetObjectItem(obj, "reset_topic");
    if (cJSON_IsString(reset) && reset->valuestring) {
        dm_str_copy(tpl->reset_topic, sizeof(tpl->reset_topic), reset->valuestring);
    }
    tpl->required_hold_ms = json_number_to_u32(cJSON_GetObjectItem(obj, "required_hold_ms"), tpl->required_hold_ms);
    tpl->heartbeat_timeout_ms = json_number_to_u32(cJSON_GetObjectItem(obj, "heartbeat_timeout_ms"), tpl->heartbeat_timeout_ms);
    const cJSON *hold_track = cJSON_GetObjectItem(obj, "hold_track");
    if (cJSON_IsString(hold_track) && hold_track->valuestring) {
        dm_str_copy(tpl->hold_track, sizeof(tpl->hold_track), hold_track->valuestring);
    }
    const cJSON *hold_loop = cJSON_GetObjectItem(obj, "hold_track_loop");
    if (cJSON_IsBool(hold_loop)) {
        tpl->hold_track_loop = cJSON_IsTrue(hold_loop);
    }
    const cJSON *complete = cJSON_GetObjectItem(obj, "complete_track");
    if (cJSON_IsString(complete) && complete->valuestring) {
        dm_str_copy(tpl->complete_track, sizeof(tpl->complete_track), complete->valuestring);
    }
    return tpl->signal_topic[0] && tpl->heartbeat_topic[0] && tpl->required_hold_ms > 0;
}

// Parse MQTT trigger rules from JSON array.
static bool mqtt_trigger_from_json(dm_mqtt_trigger_template_t *tpl, const cJSON *obj)
{
    if (!tpl || !obj) {
        return false;
    }
    dm_mqtt_trigger_template_clear(tpl);
    const cJSON *rules = cJSON_GetObjectItem(obj, "rules");
    if (!cJSON_IsArray(rules)) {
        return false;
    }
    uint8_t count = 0;
    const cJSON *rule_obj = NULL;
    cJSON_ArrayForEach(rule_obj, rules) {
        if (count >= DM_MQTT_TRIGGER_MAX_RULES) {
            break;
        }
        if (!cJSON_IsObject(rule_obj)) {
            continue;
        }
        const cJSON *topic = cJSON_GetObjectItem(rule_obj, "topic");
        const cJSON *scenario = cJSON_GetObjectItem(rule_obj, "scenario");
        if (!cJSON_IsString(topic) || !topic->valuestring || !topic->valuestring[0] ||
            !cJSON_IsString(scenario) || !scenario->valuestring || !scenario->valuestring[0]) {
            continue;
        }
        dm_mqtt_trigger_rule_t *rule = &tpl->rules[count];
        memset(rule, 0, sizeof(*rule));
        dm_str_copy(rule->topic, sizeof(rule->topic), topic->valuestring);
        dm_str_copy(rule->scenario, sizeof(rule->scenario), scenario->valuestring);
        const cJSON *name = cJSON_GetObjectItem(rule_obj, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            dm_str_copy(rule->name, sizeof(rule->name), name->valuestring);
        }
        const cJSON *payload = cJSON_GetObjectItem(rule_obj, "payload");
        if (cJSON_IsString(payload) && payload->valuestring) {
            dm_str_copy(rule->payload, sizeof(rule->payload), payload->valuestring);
        }
        rule->payload_required = json_get_bool_default(cJSON_GetObjectItem(rule_obj, "payload_required"),
                                                       rule->payload[0] != 0);
        count++;
    }
    tpl->rule_count = count;
    return tpl->rule_count > 0;
}

// Parse flag-trigger template.
static bool flag_trigger_from_json(dm_flag_trigger_template_t *tpl, const cJSON *obj)
{
    if (!tpl || !obj) {
        return false;
    }
    dm_flag_trigger_template_clear(tpl);
    const cJSON *rules = cJSON_GetObjectItem(obj, "rules");
    if (!cJSON_IsArray(rules)) {
        return false;
    }
    uint8_t count = 0;
    const cJSON *rule_obj = NULL;
    cJSON_ArrayForEach(rule_obj, rules) {
        if (count >= DM_FLAG_TRIGGER_MAX_RULES) {
            break;
        }
        if (!cJSON_IsObject(rule_obj)) {
            continue;
        }
        const cJSON *flag = cJSON_GetObjectItem(rule_obj, "flag");
        const cJSON *scenario = cJSON_GetObjectItem(rule_obj, "scenario");
        if (!cJSON_IsString(flag) || !flag->valuestring || !flag->valuestring[0] ||
            !cJSON_IsString(scenario) || !scenario->valuestring || !scenario->valuestring[0]) {
            continue;
        }
        dm_flag_trigger_rule_t *rule = &tpl->rules[count];
        memset(rule, 0, sizeof(*rule));
        dm_str_copy(rule->flag, sizeof(rule->flag), flag->valuestring);
        dm_str_copy(rule->scenario, sizeof(rule->scenario), scenario->valuestring);
        const cJSON *name = cJSON_GetObjectItem(rule_obj, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            dm_str_copy(rule->name, sizeof(rule->name), name->valuestring);
        }
        rule->required_state = json_get_bool_default(cJSON_GetObjectItem(rule_obj, "state"), true);
        count++;
    }
    tpl->rule_count = count;
    return tpl->rule_count > 0;
}

// Parse conditional scenario template (if/then behavior).
static bool condition_template_from_json(dm_condition_template_t *tpl, const cJSON *obj)
{
    if (!tpl || !obj) {
        return false;
    }
    dm_condition_template_clear(tpl);
    const cJSON *mode = cJSON_GetObjectItem(obj, "mode");
    if (cJSON_IsString(mode) && mode->valuestring) {
        if (!dm_condition_from_string(mode->valuestring, &tpl->mode)) {
            tpl->mode = DEVICE_CONDITION_ALL;
        }
    }
    const cJSON *true_sc = cJSON_GetObjectItem(obj, "true_scenario");
    if (cJSON_IsString(true_sc) && true_sc->valuestring) {
        dm_str_copy(tpl->true_scenario, sizeof(tpl->true_scenario), true_sc->valuestring);
    }
    const cJSON *false_sc = cJSON_GetObjectItem(obj, "false_scenario");
    if (cJSON_IsString(false_sc) && false_sc->valuestring) {
        dm_str_copy(tpl->false_scenario, sizeof(tpl->false_scenario), false_sc->valuestring);
    }
    const cJSON *rules = cJSON_GetObjectItem(obj, "rules");
    if (!cJSON_IsArray(rules)) {
        return false;
    }
    uint8_t count = 0;
    const cJSON *rule = NULL;
    cJSON_ArrayForEach(rule, rules) {
        if (count >= DM_CONDITION_TEMPLATE_MAX_RULES) {
            break;
        }
        if (!cJSON_IsObject(rule)) {
            continue;
        }
        const cJSON *flag = cJSON_GetObjectItem(rule, "flag");
        if (!cJSON_IsString(flag) || !flag->valuestring[0]) {
            continue;
        }
        dm_condition_rule_t *dst = &tpl->rules[count++];
        dm_str_copy(dst->flag, sizeof(dst->flag), flag->valuestring);
        dst->required_state = json_get_bool_default(cJSON_GetObjectItem(rule, "state"), true);
    }
    tpl->rule_count = count;
    return tpl->rule_count > 0 && tpl->true_scenario[0];
}

// Parse periodic task template from JSON.
static bool interval_template_from_json(dm_interval_task_template_t *tpl, const cJSON *obj)
{
    if (!tpl || !obj) {
        return false;
    }
    dm_interval_task_template_clear(tpl);
    tpl->interval_ms = json_number_to_u32(cJSON_GetObjectItem(obj, "interval_ms"), tpl->interval_ms);
    const cJSON *scenario = cJSON_GetObjectItem(obj, "scenario");
    if (cJSON_IsString(scenario) && scenario->valuestring) {
        dm_str_copy(tpl->scenario, sizeof(tpl->scenario), scenario->valuestring);
    }
    return tpl->interval_ms > 0 && tpl->scenario[0];
}

// Parse ordered-sequence template (new puzzle) from JSON.
static bool sequence_template_from_json(dm_sequence_template_t *tpl, const cJSON *obj)
{
    if (!tpl || !obj) {
        return false;
    }
    dm_sequence_template_clear(tpl);
    const cJSON *steps = cJSON_GetObjectItem(obj, "steps");
    if (!cJSON_IsArray(steps)) {
        return false;
    }
    uint8_t step_index = 0;
    const cJSON *step_obj = NULL;
    cJSON_ArrayForEach(step_obj, steps) {
        if (step_index >= DM_SEQUENCE_TEMPLATE_MAX_STEPS) {
            break;
        }
        if (!cJSON_IsObject(step_obj)) {
            continue;
        }
        const cJSON *topic = cJSON_GetObjectItem(step_obj, "topic");
        if (!cJSON_IsString(topic) || !topic->valuestring || !topic->valuestring[0]) {
            continue;
        }
        dm_sequence_step_t *step = &tpl->steps[step_index];
        memset(step, 0, sizeof(*step));
        dm_str_copy(step->topic, sizeof(step->topic), topic->valuestring);
        const cJSON *payload = cJSON_GetObjectItem(step_obj, "payload");
        if (cJSON_IsString(payload) && payload->valuestring) {
            dm_str_copy(step->payload, sizeof(step->payload), payload->valuestring);
        }
        step->payload_required =
            json_get_bool_default(cJSON_GetObjectItem(step_obj, "payload_required"), step->payload_required);
        const cJSON *hint_topic = cJSON_GetObjectItem(step_obj, "hint_topic");
        if (cJSON_IsString(hint_topic) && hint_topic->valuestring) {
            dm_str_copy(step->hint_topic, sizeof(step->hint_topic), hint_topic->valuestring);
        }
        const cJSON *hint_payload = cJSON_GetObjectItem(step_obj, "hint_payload");
        if (cJSON_IsString(hint_payload) && hint_payload->valuestring) {
            dm_str_copy(step->hint_payload, sizeof(step->hint_payload), hint_payload->valuestring);
        }
        const cJSON *hint_audio = cJSON_GetObjectItem(step_obj, "hint_audio_track");
        if (cJSON_IsString(hint_audio) && hint_audio->valuestring) {
            dm_str_copy(step->hint_audio_track, sizeof(step->hint_audio_track), hint_audio->valuestring);
        }
        step_index++;
    }
    tpl->step_count = step_index;
    tpl->timeout_ms = json_number_to_u32(cJSON_GetObjectItem(obj, "timeout_ms"), tpl->timeout_ms);
    tpl->reset_on_error =
        json_get_bool_default(cJSON_GetObjectItem(obj, "reset_on_error"), tpl->reset_on_error);
    const cJSON *success_topic = cJSON_GetObjectItem(obj, "success_topic");
    if (cJSON_IsString(success_topic) && success_topic->valuestring) {
        dm_str_copy(tpl->success_topic, sizeof(tpl->success_topic), success_topic->valuestring);
    }
    const cJSON *success_payload = cJSON_GetObjectItem(obj, "success_payload");
    if (cJSON_IsString(success_payload) && success_payload->valuestring) {
        dm_str_copy(tpl->success_payload, sizeof(tpl->success_payload), success_payload->valuestring);
    }
    const cJSON *success_audio = cJSON_GetObjectItem(obj, "success_audio_track");
    if (cJSON_IsString(success_audio) && success_audio->valuestring) {
        dm_str_copy(tpl->success_audio_track, sizeof(tpl->success_audio_track), success_audio->valuestring);
    }
    const cJSON *success_scenario = cJSON_GetObjectItem(obj, "success_scenario");
    if (cJSON_IsString(success_scenario) && success_scenario->valuestring) {
        dm_str_copy(tpl->success_scenario, sizeof(tpl->success_scenario), success_scenario->valuestring);
    }
    const cJSON *fail_topic = cJSON_GetObjectItem(obj, "fail_topic");
    if (cJSON_IsString(fail_topic) && fail_topic->valuestring) {
        dm_str_copy(tpl->fail_topic, sizeof(tpl->fail_topic), fail_topic->valuestring);
    }
    const cJSON *fail_payload = cJSON_GetObjectItem(obj, "fail_payload");
    if (cJSON_IsString(fail_payload) && fail_payload->valuestring) {
        dm_str_copy(tpl->fail_payload, sizeof(tpl->fail_payload), fail_payload->valuestring);
    }
    const cJSON *fail_audio = cJSON_GetObjectItem(obj, "fail_audio_track");
    if (cJSON_IsString(fail_audio) && fail_audio->valuestring) {
        dm_str_copy(tpl->fail_audio_track, sizeof(tpl->fail_audio_track), fail_audio->valuestring);
    }
    const cJSON *fail_scenario = cJSON_GetObjectItem(obj, "fail_scenario");
    if (cJSON_IsString(fail_scenario) && fail_scenario->valuestring) {
        dm_str_copy(tpl->fail_scenario, sizeof(tpl->fail_scenario), fail_scenario->valuestring);
    }
    return tpl->step_count > 0;
}

// Dispatch helper: parse template payload based on requested type.
static bool template_from_json(device_descriptor_t *dev, const cJSON *obj)
{
    if (!dev || !obj) {
        dev->template_assigned = false;
        return false;
    }
    const cJSON *type_item = cJSON_GetObjectItem(obj, "type");
    if (!cJSON_IsString(type_item) || !type_item->valuestring) {
        dev->template_assigned = false;
        return false;
    }
    dm_template_type_t type;
    if (!dm_template_type_from_string(type_item->valuestring, &type)) {
        dev->template_assigned = false;
        return false;
    }
    dev->template_assigned = true;
    dev->template_config.type = type;
    bool ok = false;
    switch (type) {
    case DM_TEMPLATE_TYPE_UID: {
        const cJSON *uid_obj = cJSON_GetObjectItem(obj, "uid");
        ok = uid_template_from_json(&dev->template_config.data.uid, uid_obj);
        break;
    }
    case DM_TEMPLATE_TYPE_SIGNAL_HOLD: {
        const cJSON *sig_obj = cJSON_GetObjectItem(obj, "signal");
        ok = signal_template_from_json(&dev->template_config.data.signal, sig_obj);
        break;
    }
    case DM_TEMPLATE_TYPE_MQTT_TRIGGER: {
        const cJSON *mqtt_obj = cJSON_GetObjectItem(obj, "mqtt");
        ok = mqtt_trigger_from_json(&dev->template_config.data.mqtt, mqtt_obj);
        break;
    }
    case DM_TEMPLATE_TYPE_FLAG_TRIGGER: {
        const cJSON *flag_obj = cJSON_GetObjectItem(obj, "flag");
        ok = flag_trigger_from_json(&dev->template_config.data.flag, flag_obj);
        break;
    }
    case DM_TEMPLATE_TYPE_IF_CONDITION: {
        const cJSON *cond_obj = cJSON_GetObjectItem(obj, "condition");
        ok = condition_template_from_json(&dev->template_config.data.condition, cond_obj);
        break;
    }
    case DM_TEMPLATE_TYPE_INTERVAL_TASK: {
        const cJSON *interval_obj = cJSON_GetObjectItem(obj, "interval");
        ok = interval_template_from_json(&dev->template_config.data.interval, interval_obj);
        break;
    }
    case DM_TEMPLATE_TYPE_SEQUENCE_LOCK: {
        const cJSON *sequence_obj = cJSON_GetObjectItem(obj, "sequence");
        ok = sequence_template_from_json(&dev->template_config.data.sequence, sequence_obj);
        break;
    }
    default:
        ok = false;
        break;
    }
    if (!ok) {
        dev->template_assigned = false;
    }
    return dev->template_assigned;
}

// Parse single scenario step (MQTT/audio/flags/etc).
static bool step_from_json(const cJSON *obj, device_action_step_t *step)
{
    if (!obj || !step) {
        return false;
    }
    const cJSON *type_item = cJSON_GetObjectItem(obj, "type");
    if (!cJSON_IsString(type_item)) {
        return false;
    }
    device_action_type_t type;
    if (!dm_action_type_from_string(type_item->valuestring, &type)) {
        return false;
    }
    memset(step, 0, sizeof(*step));
    step->type = type;
    step->delay_ms = json_number_to_u32(cJSON_GetObjectItem(obj, "delay_ms"), 0);
    switch (type) {
    case DEVICE_ACTION_MQTT_PUBLISH:
        dm_str_copy(step->data.mqtt.topic, sizeof(step->data.mqtt.topic),
                    cJSON_GetStringValue(cJSON_GetObjectItem(obj, "topic")));
        dm_str_copy(step->data.mqtt.payload, sizeof(step->data.mqtt.payload),
                    cJSON_GetStringValue(cJSON_GetObjectItem(obj, "payload")));
        step->data.mqtt.qos = (uint8_t)json_number_to_u32(cJSON_GetObjectItem(obj, "qos"), 0);
        step->data.mqtt.retain = json_get_bool_default(cJSON_GetObjectItem(obj, "retain"), false);
        break;
    case DEVICE_ACTION_AUDIO_PLAY:
        dm_str_copy(step->data.audio.track, sizeof(step->data.audio.track),
                    cJSON_GetStringValue(cJSON_GetObjectItem(obj, "track")));
        step->data.audio.blocking = json_get_bool_default(cJSON_GetObjectItem(obj, "blocking"), false);
        break;
    case DEVICE_ACTION_SET_FLAG:
        dm_str_copy(step->data.flag.flag, sizeof(step->data.flag.flag),
                    cJSON_GetStringValue(cJSON_GetObjectItem(obj, "flag")));
        step->data.flag.value = json_get_bool_default(cJSON_GetObjectItem(obj, "value"), false);
        break;
    case DEVICE_ACTION_WAIT_FLAGS: {
        const cJSON *wait = cJSON_GetObjectItem(obj, "wait");
        if (!cJSON_IsObject(wait)) {
            return false;
        }
        const cJSON *mode = cJSON_GetObjectItem(wait, "mode");
        if (!mode || !cJSON_IsString(mode) ||
            !dm_condition_from_string(mode->valuestring, &step->data.wait_flags.mode)) {
            step->data.wait_flags.mode = DEVICE_CONDITION_ALL;
        }
        step->data.wait_flags.timeout_ms = json_number_to_u32(cJSON_GetObjectItem(wait, "timeout_ms"), 0);
        const cJSON *reqs = cJSON_GetObjectItem(wait, "requirements");
        uint8_t req_count = 0;
        if (cJSON_IsArray(reqs)) {
            const cJSON *req = NULL;
            cJSON_ArrayForEach(req, reqs) {
                if (req_count >= DEVICE_MANAGER_MAX_FLAG_RULES) {
                    break;
                }
                const cJSON *flag = cJSON_GetObjectItem(req, "flag");
                if (!cJSON_IsString(flag)) {
                    continue;
                }
                device_flag_requirement_t *dst = &step->data.wait_flags.requirements[req_count++];
                dm_str_copy(dst->flag, sizeof(dst->flag), flag->valuestring);
                dst->required_state = json_get_bool_default(cJSON_GetObjectItem(req, "state"), true);
            }
        }
        step->data.wait_flags.requirement_count = req_count;
        break;
    }
    case DEVICE_ACTION_LOOP: {
        const cJSON *loop = cJSON_GetObjectItem(obj, "loop");
        if (!cJSON_IsObject(loop)) {
            return false;
        }
        step->data.loop.target_step = json_number_to_u16(cJSON_GetObjectItem(loop, "target_step"), 0);
        step->data.loop.max_iterations = json_number_to_u16(cJSON_GetObjectItem(loop, "max_iterations"), 0);
        break;
    }
    case DEVICE_ACTION_EVENT_BUS:
        dm_str_copy(step->data.event.event, sizeof(step->data.event.event),
                    cJSON_GetStringValue(cJSON_GetObjectItem(obj, "event")));
        dm_str_copy(step->data.event.topic, sizeof(step->data.event.topic),
                    cJSON_GetStringValue(cJSON_GetObjectItem(obj, "topic")));
        dm_str_copy(step->data.event.payload, sizeof(step->data.event.payload),
                    cJSON_GetStringValue(cJSON_GetObjectItem(obj, "payload")));
        break;
    case DEVICE_ACTION_AUDIO_STOP:
    case DEVICE_ACTION_DELAY:
    case DEVICE_ACTION_NOP:
    default:
        break;
    }
    return true;
}

void dm_load_defaults(device_manager_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    uint8_t capacity = cfg->device_capacity;
    device_descriptor_t *devices = cfg->devices;
    if (devices && capacity > 0) {
        memset(devices, 0, sizeof(device_descriptor_t) * capacity);
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->device_capacity = capacity;
    cfg->schema_version = DM_DEVICE_CONFIG_VERSION;
    cfg->generation = 1;
    cfg->profile_count = 0;
    cfg->active_profile[0] = 0;
    dm_profiles_ensure_active(cfg);
}

// Convert JSON tree into in-memory device_manager_config_t.
bool dm_populate_config_from_json(device_manager_config_t *cfg, const cJSON *root)
{
    if (!cfg || !root) {
        return false;
    }
    dm_load_defaults(cfg);
    cfg->schema_version = json_number_to_u32(cJSON_GetObjectItem(root, "schema"), DM_DEVICE_CONFIG_VERSION);
    cfg->generation = json_number_to_u32(cJSON_GetObjectItem(root, "generation"), cfg->generation);
    cfg->profile_count = 0;
    cfg->active_profile[0] = 0;
    // Profiles array only stores metadata and device_count, actual devices parsed below.
    const cJSON *profiles = cJSON_GetObjectItem(root, "profiles");
    if (cJSON_IsArray(profiles)) {
        const cJSON *node = NULL;
        cJSON_ArrayForEach(node, profiles) {
            if (cfg->profile_count >= DEVICE_MANAGER_MAX_PROFILES) {
                break;
            }
            if (!cJSON_IsObject(node)) {
                continue;
            }
            const cJSON *id_item = cJSON_GetObjectItem(node, "id");
            if (!cJSON_IsString(id_item) || !id_item->valuestring[0]) {
                continue;
            }
            device_manager_profile_t *profile = dm_profiles_find_by_id(cfg, id_item->valuestring);
            if (!profile && cfg->profile_count < DEVICE_MANAGER_MAX_PROFILES) {
                profile = &cfg->profiles[cfg->profile_count++];
                memset(profile, 0, sizeof(*profile));
                dm_str_copy(profile->id, sizeof(profile->id), id_item->valuestring);
            }
            if (!profile) {
                continue;
            }
            const cJSON *name_item = cJSON_GetObjectItem(node, "name");
            if (cJSON_IsString(name_item) && name_item->valuestring) {
                dm_str_copy(profile->name, sizeof(profile->name), name_item->valuestring);
            }
            const cJSON *count_item = cJSON_GetObjectItem(node, "device_count");
            if (cJSON_IsNumber(count_item)) {
                uint32_t cnt = json_number_to_u32(count_item, 0);
                uint8_t cap = cfg->device_capacity ? cfg->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
                profile->device_count = (cnt > cap) ? cap : (uint8_t)cnt;
            }
        }
    }
    const cJSON *active = cJSON_GetObjectItem(root, "active_profile");
    if (cJSON_IsString(active) && active->valuestring && active->valuestring[0]) {
        dm_str_copy(cfg->active_profile, sizeof(cfg->active_profile), active->valuestring);
    }
    dm_profiles_ensure_active(cfg);

    // Parse devices with topics/scenarios/template payloads.
    const cJSON *devices = cJSON_GetObjectItem(root, "devices");
    if (!devices || !cJSON_IsArray(devices)) {
        cfg->device_count = 0;
        return true;
    }
    uint8_t dev_count = 0;
    const cJSON *dev_node = NULL;
    cJSON_ArrayForEach(dev_node, devices) {
        if (cfg->device_capacity && dev_count >= cfg->device_capacity) {
            break;
        }
        if (!cJSON_IsObject(dev_node)) {
            continue;
        }
        device_descriptor_t *dev = &cfg->devices[dev_count];
        memset(dev, 0, sizeof(*dev));
        dm_str_copy(dev->id, sizeof(dev->id), cJSON_GetStringValue(cJSON_GetObjectItem(dev_node, "id")));
        const cJSON *display_name = cJSON_GetObjectItem(dev_node, "display_name");
        const cJSON *name = cJSON_GetObjectItem(dev_node, "name");
        const char *display = NULL;
        if (cJSON_IsString(display_name) && display_name->valuestring) {
            display = display_name->valuestring;
        } else {
            display = cJSON_GetStringValue(name);
        }
        if (!display || !display[0]) {
            display = dev->id;
        }
        dm_str_copy(dev->display_name, sizeof(dev->display_name), display);
        feed_wdt();

        const cJSON *topics = cJSON_GetObjectItem(dev_node, "topics");
        uint8_t topic_count = 0;
        if (cJSON_IsArray(topics)) {
            const cJSON *topic_node = NULL;
            cJSON_ArrayForEach(topic_node, topics) {
                if (topic_count >= DEVICE_MANAGER_MAX_TOPICS_PER_DEVICE) {
                    break;
                }
                if (!cJSON_IsObject(topic_node)) {
                    continue;
                }
                device_topic_binding_t *binding = &dev->topics[topic_count++];
                dm_str_copy(binding->name, sizeof(binding->name),
                            cJSON_GetStringValue(cJSON_GetObjectItem(topic_node, "name")));
                dm_str_copy(binding->topic, sizeof(binding->topic),
                            cJSON_GetStringValue(cJSON_GetObjectItem(topic_node, "topic")));
                feed_wdt();
            }
        }
        dev->topic_count = topic_count;

        const cJSON *scenarios = cJSON_GetObjectItem(dev_node, "scenarios");
        uint8_t scenario_count = 0;
        if (cJSON_IsArray(scenarios)) {
            const cJSON *sc_node = NULL;
            cJSON_ArrayForEach(sc_node, scenarios) {
                if (scenario_count >= DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE) {
                    break;
                }
                if (!cJSON_IsObject(sc_node)) {
                    continue;
                }
                device_scenario_t *sc = &dev->scenarios[scenario_count];
                memset(sc, 0, sizeof(*sc));
                dm_str_copy(sc->id, sizeof(sc->id), cJSON_GetStringValue(cJSON_GetObjectItem(sc_node, "id")));
                dm_str_copy(sc->name, sizeof(sc->name), cJSON_GetStringValue(cJSON_GetObjectItem(sc_node, "name")));
                const cJSON *button = cJSON_GetObjectItem(sc_node, "button_enabled");
                sc->button_enabled = cJSON_IsTrue(button);
                dm_str_copy(sc->button_label, sizeof(sc->button_label),
                            cJSON_GetStringValue(cJSON_GetObjectItem(sc_node, "button_label")));
                const cJSON *steps = cJSON_GetObjectItem(sc_node, "steps");
                uint8_t step_count = 0;
                if (cJSON_IsArray(steps)) {
                    const cJSON *step_node = NULL;
                    cJSON_ArrayForEach(step_node, steps) {
                        if (step_count >= DEVICE_MANAGER_MAX_STEPS_PER_SCENARIO) {
                            break;
                        }
                        if (!cJSON_IsObject(step_node)) {
                            continue;
                        }
                        if (!step_from_json(step_node, &sc->steps[step_count])) {
                            ESP_LOGW(TAG, "invalid step skipped in scenario %s", sc->id);
                            continue;
                        }
                        step_count++;
                        feed_wdt();
                    }
                }
                sc->step_count = step_count;
                scenario_count++;
                feed_wdt();
            }
        }
        dev->scenario_count = scenario_count;

        const cJSON *template_obj = cJSON_GetObjectItem(dev_node, "template");
        if (cJSON_IsObject(template_obj)) {
            if (!template_from_json(dev, template_obj)) {
                ESP_LOGW(TAG, "invalid template for device %s, ignoring", dev->id);
                dev->template_assigned = false;
            }
        } else {
            dev->template_assigned = false;
        }

        dev_count++;
        feed_wdt();
    }
    cfg->device_count = dev_count;
    dm_profiles_sync_to_active(cfg);
    return true;
}

esp_err_t dm_storage_internal_parse(const char *json, size_t len, device_manager_config_t *cfg)
{
    if (!json || len == 0 || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    dm_cjson_install_hooks();
    cJSON *root = cJSON_ParseWithLength(json, len);
    dm_cjson_reset_hooks();
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    bool ok = dm_populate_config_from_json(cfg, root);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}
