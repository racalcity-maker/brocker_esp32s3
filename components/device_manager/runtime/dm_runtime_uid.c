#include "dm_runtime_uid.h"

#include <string.h>
#include <ctype.h>

#include "esp_log.h"

#include "device_manager_utils.h"

static const char *TAG = "dm_runtime_uid";

static void fill_action(const dm_uid_template_t *tpl, bool success, dm_uid_action_t *action)
{
    if (!tpl || !action) {
        return;
    }
    const char *channel_topic = success ? tpl->success_topic : tpl->fail_topic;
    const char *channel_payload = success ? tpl->success_payload : tpl->fail_payload;
    const char *signal_topic = success ? tpl->success_signal_topic : tpl->fail_signal_topic;
    const char *signal_payload = success ? tpl->success_signal_payload : tpl->fail_signal_payload;
    const char *audio_track = success ? tpl->success_audio_track : tpl->fail_audio_track;

    if (channel_topic && channel_topic[0]) {
        action->publish_channel = true;
        dm_str_copy(action->channel_topic, sizeof(action->channel_topic), channel_topic);
        dm_str_copy(action->channel_payload, sizeof(action->channel_payload), channel_payload);
    }
    if (signal_topic && signal_topic[0]) {
        action->publish_signal = true;
        dm_str_copy(action->signal_topic, sizeof(action->signal_topic), signal_topic);
        dm_str_copy(action->signal_payload, sizeof(action->signal_payload), signal_payload);
    }
    if (audio_track && audio_track[0]) {
        action->audio_play = true;
        dm_str_copy(action->audio_track, sizeof(action->audio_track), audio_track);
    }
}

void dm_uid_runtime_init(dm_uid_runtime_t *rt, const dm_uid_template_t *tpl)
{
    if (!rt) {
        return;
    }
    if (tpl) {
        memcpy(&rt->config, tpl, sizeof(*tpl));
    } else {
        memset(&rt->config, 0, sizeof(rt->config));
    }
    dm_uid_state_reset(&rt->state);
    memset(rt->slots, 0, sizeof(rt->slots));
}

void dm_uid_runtime_set_template(dm_uid_runtime_t *rt, const dm_uid_template_t *tpl)
{
    if (!rt || !tpl) {
        return;
    }
    memcpy(&rt->config, tpl, sizeof(*tpl));
    dm_uid_state_reset(&rt->state);
    memset(rt->slots, 0, sizeof(rt->slots));
}

static const char *uid_event_str(dm_uid_event_type_t type)
{
    switch (type) {
    case DM_UID_EVENT_ACCEPTED:
        return "accepted";
    case DM_UID_EVENT_DUPLICATE:
        return "duplicate";
    case DM_UID_EVENT_INVALID:
        return "invalid";
    case DM_UID_EVENT_SUCCESS:
        return "success";
    default:
        return "none";
    }
}

static void sanitize_uid_value(const char *input, char *output, size_t output_len)
{
    if (!output || output_len == 0) {
        return;
    }
    size_t pos = 0;
    if (!input) {
        input = "";
    }
    while (*input && isspace((unsigned char)*input)) {
        ++input;
    }
    while (*input && pos < output_len - 1) {
        output[pos++] = *input++;
    }
    while (pos > 0 && isspace((unsigned char)output[pos - 1])) {
        --pos;
    }
    output[pos] = '\0';
}

dm_uid_action_t dm_uid_runtime_handle_value(dm_uid_runtime_t *rt,
                                            const char *source_id,
                                            const char *value)
{
    dm_uid_action_t action = {0};
    if (!rt) {
        return action;
    }
    char cleaned_value[DM_UID_TEMPLATE_VALUE_MAX_LEN];
    sanitize_uid_value(value, cleaned_value, sizeof(cleaned_value));
    ESP_LOGI(TAG, "incoming source='%s' raw='%s' clean='%s'",
             source_id ? source_id : "(null)",
             value ? value : "",
             cleaned_value);
    dm_uid_event_t ev = dm_uid_handle_value(&rt->state, &rt->config, source_id, cleaned_value);
    if (ev.slot) {
        int idx = (int)(ev.slot - rt->config.slots);
        if (idx >= 0 && idx < DM_UID_TEMPLATE_MAX_SLOTS) {
            if (cleaned_value[0]) {
                dm_str_copy(rt->slots[idx].value, sizeof(rt->slots[idx].value), cleaned_value);
                rt->slots[idx].has_value = true;
            } else {
                rt->slots[idx].value[0] = '\0';
                rt->slots[idx].has_value = false;
            }
            ESP_LOGI(TAG, "slot[%d] label='%s' event=%s stored='%s' ok=%u/%u failed=%d",
                     idx,
                     ev.slot->label[0] ? ev.slot->label : ev.slot->source_id,
                     uid_event_str(ev.type),
                     rt->slots[idx].has_value ? rt->slots[idx].value : "",
                     (unsigned)rt->state.ok_count,
                     (unsigned)rt->config.slot_count,
                     rt->state.failed);
        }
    } else {
        ESP_LOGW(TAG, "source '%s' not found in template", source_id ? source_id : "(null)");
    }
    action.event = ev.type;
    switch (ev.type) {
    case DM_UID_EVENT_INVALID:
        fill_action(&rt->config, false, &action);
        dm_uid_state_reset(&rt->state);
        break;
    case DM_UID_EVENT_SUCCESS:
        fill_action(&rt->config, true, &action);
        dm_uid_state_reset(&rt->state);
        break;
    default:
        break;
    }
    return action;
}
