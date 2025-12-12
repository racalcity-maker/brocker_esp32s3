#include "dm_template_factory.h"

#include <string.h>

#include "device_manager_utils.h"

static esp_err_t copy_topic_payload(char *topic,
                                    size_t topic_len,
                                    char *payload,
                                    size_t payload_len,
                                    const char *src_topic,
                                    const char *src_payload)
{
    if (src_topic && src_topic[0]) {
        dm_str_copy(topic, topic_len, src_topic);
        if (src_payload) {
            dm_str_copy(payload, payload_len, src_payload);
        } else {
            payload[0] = '\0';
        }
    } else {
        topic[0] = '\0';
        if (payload_len > 0) {
            payload[0] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t dm_template_factory_build_uid(const dm_uid_template_params_t *params, dm_template_config_t *out)
{
    if (!params || !out || !params->slots || params->slot_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (params->slot_count > DM_UID_TEMPLATE_MAX_SLOTS) {
        return ESP_ERR_INVALID_SIZE;
    }
    out->type = DM_TEMPLATE_TYPE_UID;
    dm_uid_template_clear(&out->data.uid);

    for (size_t i = 0; i < params->slot_count; ++i) {
        const dm_uid_slot_param_t *slot = &params->slots[i];
        if (!slot || !slot->source_id || !slot->source_id[0] || !slot->values || slot->value_count == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!dm_uid_template_set_slot(&out->data.uid, (uint8_t)i, slot->source_id, slot->label)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (slot->value_count > DM_UID_TEMPLATE_MAX_VALUES) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t v = 0; v < slot->value_count; ++v) {
            const char *value = slot->values[v];
            if (!value || !value[0]) {
                return ESP_ERR_INVALID_ARG;
            }
            if (!dm_uid_template_add_value(&out->data.uid, (uint8_t)i, value)) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    copy_topic_payload(out->data.uid.start_topic,
                       sizeof(out->data.uid.start_topic),
                       out->data.uid.start_payload,
                       sizeof(out->data.uid.start_payload),
                       params->start_topic,
                       params->start_payload);
    copy_topic_payload(out->data.uid.broadcast_topic,
                       sizeof(out->data.uid.broadcast_topic),
                       out->data.uid.broadcast_payload,
                       sizeof(out->data.uid.broadcast_payload),
                       params->broadcast_topic,
                       params->broadcast_payload);
    copy_topic_payload(out->data.uid.success_topic,
                       sizeof(out->data.uid.success_topic),
                       out->data.uid.success_payload,
                       sizeof(out->data.uid.success_payload),
                       params->success_topic,
                       params->success_payload);
    copy_topic_payload(out->data.uid.fail_topic,
                       sizeof(out->data.uid.fail_topic),
                       out->data.uid.fail_payload,
                       sizeof(out->data.uid.fail_payload),
                       params->fail_topic,
                       params->fail_payload);
    copy_topic_payload(out->data.uid.success_signal_topic,
                       sizeof(out->data.uid.success_signal_topic),
                       out->data.uid.success_signal_payload,
                       sizeof(out->data.uid.success_signal_payload),
                       params->success_signal_topic,
                       params->success_signal_payload);
    copy_topic_payload(out->data.uid.fail_signal_topic,
                       sizeof(out->data.uid.fail_signal_topic),
                       out->data.uid.fail_signal_payload,
                       sizeof(out->data.uid.fail_signal_payload),
                       params->fail_signal_topic,
                       params->fail_signal_payload);

    dm_str_copy(out->data.uid.success_audio_track,
                sizeof(out->data.uid.success_audio_track),
                params->success_audio_track);
    dm_str_copy(out->data.uid.fail_audio_track,
                sizeof(out->data.uid.fail_audio_track),
                params->fail_audio_track);

    return ESP_OK;
}

esp_err_t dm_template_factory_build_signal_hold(const dm_signal_hold_params_t *params, dm_template_config_t *out)
{
    if (!params || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!params->signal_topic || !params->signal_topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!params->heartbeat_topic || !params->heartbeat_topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (params->required_hold_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out->type = DM_TEMPLATE_TYPE_SIGNAL_HOLD;
    dm_signal_template_clear(&out->data.signal);

    dm_str_copy(out->data.signal.signal_topic, sizeof(out->data.signal.signal_topic), params->signal_topic);
    dm_str_copy(out->data.signal.signal_payload_on,
                sizeof(out->data.signal.signal_payload_on),
                params->signal_payload_on);
    dm_str_copy(out->data.signal.signal_payload_off,
                sizeof(out->data.signal.signal_payload_off),
                params->signal_payload_off);
    out->data.signal.signal_on_ms = params->signal_on_ms;

    dm_str_copy(out->data.signal.heartbeat_topic, sizeof(out->data.signal.heartbeat_topic), params->heartbeat_topic);
    if (params->reset_topic) {
        dm_str_copy(out->data.signal.reset_topic, sizeof(out->data.signal.reset_topic), params->reset_topic);
    }
    out->data.signal.required_hold_ms = params->required_hold_ms;
    out->data.signal.heartbeat_timeout_ms = params->heartbeat_timeout_ms;

    dm_str_copy(out->data.signal.hold_track, sizeof(out->data.signal.hold_track), params->hold_track);
    out->data.signal.hold_track_loop = params->hold_track_loop;
    dm_str_copy(out->data.signal.complete_track, sizeof(out->data.signal.complete_track), params->complete_track);

    return ESP_OK;
}
