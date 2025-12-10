#include "dm_template_runtime.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "dm_runtime_uid.h"
#include "dm_runtime_signal.h"
#include "device_manager_utils.h"
#include "audio_player.h"
#include "automation_engine.h"
#include "event_bus.h"
#include "mqtt_core.h"

#define DM_UID_RUNTIME_MAX     4
#define DM_SIGNAL_RUNTIME_MAX  4

static const char *TAG = "template_runtime";

typedef struct {
    bool in_use;
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    dm_uid_runtime_t runtime;
    char topics[DM_UID_TEMPLATE_MAX_SLOTS][DEVICE_MANAGER_TOPIC_MAX_LEN];
    size_t topic_count;
} uid_runtime_entry_t;

typedef struct {
    bool in_use;
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    dm_signal_runtime_t runtime;
    char heartbeat_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    bool hold_started;
    bool hold_paused;
    bool hold_active;
} signal_runtime_entry_t;

static uid_runtime_entry_t s_uid_entries[DM_UID_RUNTIME_MAX];
static signal_runtime_entry_t s_signal_entries[DM_SIGNAL_RUNTIME_MAX];
static bool s_event_handler_registered = false;

static void template_event_handler(const event_bus_message_t *msg)
{
    if (!msg || msg->type != EVENT_MQTT_MESSAGE) {
        return;
    }
    if (!msg->topic[0]) {
        return;
    }
    const char *payload = msg->payload[0] ? msg->payload : "";
    dm_template_runtime_handle_mqtt(msg->topic, payload);
}

static uid_runtime_entry_t *allocate_uid_entry(void)
{
    for (size_t i = 0; i < DM_UID_RUNTIME_MAX; ++i) {
        if (!s_uid_entries[i].in_use) {
            return &s_uid_entries[i];
        }
    }
    return NULL;
}

static signal_runtime_entry_t *allocate_signal_entry(void)
{
    for (size_t i = 0; i < DM_SIGNAL_RUNTIME_MAX; ++i) {
        if (!s_signal_entries[i].in_use) {
            return &s_signal_entries[i];
        }
    }
    return NULL;
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

esp_err_t dm_template_runtime_init(void)
{
    memset(s_uid_entries, 0, sizeof(s_uid_entries));
    memset(s_signal_entries, 0, sizeof(s_signal_entries));
    if (!s_event_handler_registered) {
        esp_err_t err = event_bus_register_handler(template_event_handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "event handler register failed: %s", esp_err_to_name(err));
            return err;
        }
        s_event_handler_registered = true;
    }
    return ESP_OK;
}

void dm_template_runtime_reset(void)
{
    dm_template_runtime_init();
}

static esp_err_t register_uid_runtime(const dm_uid_template_t *tpl, const char *device_id)
{
    if (!tpl || tpl->slot_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uid_runtime_entry_t *entry = allocate_uid_entry();
    if (!entry) {
        ESP_LOGE(TAG, "no slot for uid runtime");
        return ESP_ERR_NO_MEM;
    }
    memset(entry, 0, sizeof(*entry));
    entry->in_use = true;
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_uid_runtime_init(&entry->runtime, tpl);
    entry->topic_count = tpl->slot_count;
    for (uint8_t i = 0; i < tpl->slot_count && i < DM_UID_TEMPLATE_MAX_SLOTS; ++i) {
        dm_str_copy(entry->topics[i], sizeof(entry->topics[i]), tpl->slots[i].source_id);
    }
    ESP_LOGI(TAG, "registered UID runtime for device %s with %zu slots", entry->device_id, entry->topic_count);
    return ESP_OK;
}

static esp_err_t register_signal_runtime(const dm_signal_hold_template_t *tpl, const char *device_id)
{
    if (!tpl || !tpl->heartbeat_topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    signal_runtime_entry_t *entry = allocate_signal_entry();
    if (!entry) {
        ESP_LOGE(TAG, "no slot for signal runtime");
        return ESP_ERR_NO_MEM;
    }
    memset(entry, 0, sizeof(*entry));
    entry->in_use = true;
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_signal_runtime_init(&entry->runtime, tpl);
    dm_str_copy(entry->heartbeat_topic, sizeof(entry->heartbeat_topic), tpl->heartbeat_topic);
    entry->hold_started = false;
    entry->hold_paused = false;
    entry->hold_active = false;
    ESP_LOGI(TAG, "registered signal runtime for device %s topic %s", entry->device_id, entry->heartbeat_topic);
    return ESP_OK;
}

esp_err_t dm_template_runtime_register(const dm_template_config_t *tpl, const char *device_id)
{
    if (!tpl || !device_id) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (tpl->type) {
    case DM_TEMPLATE_TYPE_UID:
        return register_uid_runtime(&tpl->data.uid, device_id);
    case DM_TEMPLATE_TYPE_SIGNAL_HOLD:
        return register_signal_runtime(&tpl->data.signal, device_id);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t dm_template_runtime_get_uid_snapshot(const char *device_id, dm_uid_runtime_snapshot_t *out)
{
    if (!device_id || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < DM_UID_RUNTIME_MAX; ++i) {
        uid_runtime_entry_t *entry = &s_uid_entries[i];
        if (!entry->in_use) {
            continue;
        }
        if (strcmp(entry->device_id, device_id) != 0) {
            continue;
        }
        dm_str_copy(out->device_id, sizeof(out->device_id), entry->device_id);
        out->slot_count = entry->runtime.config.slot_count;
        if (out->slot_count > DM_UID_TEMPLATE_MAX_SLOTS) {
            out->slot_count = DM_UID_TEMPLATE_MAX_SLOTS;
        }
        for (uint8_t s = 0; s < out->slot_count; ++s) {
            const dm_uid_slot_t *slot = &entry->runtime.config.slots[s];
            dm_str_copy(out->slots[s].source_id, sizeof(out->slots[s].source_id), slot->source_id);
            dm_str_copy(out->slots[s].label, sizeof(out->slots[s].label), slot->label);
            out->slots[s].has_value = entry->runtime.slots[s].has_value;
            if (entry->runtime.slots[s].has_value) {
                dm_str_copy(out->slots[s].last_value, sizeof(out->slots[s].last_value),
                            entry->runtime.slots[s].value);
            }
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static void publish_mqtt_payload(const char *topic, const char *payload)
{
    if (!topic || !topic[0]) {
        return;
    }
    const char *body = (payload && payload[0]) ? payload : "";
    esp_err_t err = mqtt_core_publish(topic, body);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mqtt publish failed (%s): %s", topic, esp_err_to_name(err));
    }
}

static void trigger_uid_scenario(const char *device_id, const char *scenario_id)
{
    esp_err_t err = automation_engine_trigger(device_id, scenario_id);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "scenario %s/%s not found", device_id, scenario_id);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to trigger %s/%s: %s", device_id, scenario_id, esp_err_to_name(err));
    }
}

static void apply_uid_action(const dm_uid_action_t *action)
{
    if (!action) {
        return;
    }
    if (action->publish_channel) {
        publish_mqtt_payload(action->channel_topic, action->channel_payload);
    }
    if (action->publish_signal) {
        publish_mqtt_payload(action->signal_topic, action->signal_payload);
    }
    if (action->audio_play && action->audio_track[0]) {
        audio_player_play(action->audio_track);
    }
}

static bool handle_uid_message(const char *topic, const char *payload)
{
    bool handled = false;
    for (size_t i = 0; i < DM_UID_RUNTIME_MAX; ++i) {
        uid_runtime_entry_t *entry = &s_uid_entries[i];
        if (!entry->in_use) {
            continue;
        }
        for (size_t t = 0; t < entry->topic_count; ++t) {
            if (entry->topics[t][0] && strcmp(entry->topics[t], topic) == 0) {
                handled = true;
                dm_uid_action_t action = dm_uid_runtime_handle_value(&entry->runtime, topic, payload ? payload : "");
                ESP_LOGI(TAG, "[UID] dev=%s topic=%s event=%s payload='%s'",
                         entry->device_id,
                         topic,
                         uid_event_str(action.event),
                         payload ? payload : "");
                apply_uid_action(&action);
                switch (action.event) {
                case DM_UID_EVENT_SUCCESS:
                    trigger_uid_scenario(entry->device_id, "uid_success");
                    break;
                case DM_UID_EVENT_INVALID:
                    trigger_uid_scenario(entry->device_id, "uid_fail");
                    break;
                default:
                    break;
                }
            }
        }
    }
    return handled;
}

static void handle_signal_audio(signal_runtime_entry_t *entry, dm_signal_event_type_t ev)
{
    const dm_signal_hold_template_t *cfg = &entry->runtime.config;
    if (!cfg->hold_track[0]) {
        return;
    }
    switch (ev) {
    case DM_SIGNAL_EVENT_START:
        if (!entry->hold_started) {
            audio_player_play(cfg->hold_track);
            entry->hold_started = true;
            entry->hold_paused = false;
            entry->hold_active = true;
        } else if (entry->hold_paused) {
            audio_player_resume();
            entry->hold_paused = false;
            entry->hold_active = true;
        }
        break;
    case DM_SIGNAL_EVENT_STOP:
        if (entry->hold_active) {
            audio_player_pause();
            entry->hold_paused = true;
            entry->hold_active = false;
        }
        break;
    case DM_SIGNAL_EVENT_COMPLETED:
        if (entry->hold_active || entry->hold_paused) {
            audio_player_stop();
        }
        entry->hold_started = false;
        entry->hold_paused = false;
        entry->hold_active = false;
        if (cfg->complete_track[0]) {
            audio_player_play(cfg->complete_track);
        }
        break;
    default:
        break;
    }
}

static void apply_signal_mqtt_action(const dm_signal_action_t *action)
{
    if (!action) {
        return;
    }
    if (action->signal_on) {
        publish_mqtt_payload(action->signal_topic, action->signal_payload_on);
    }
    if (action->signal_off) {
        publish_mqtt_payload(action->signal_topic, action->signal_payload_off);
    }
}

static bool handle_signal_message(const char *topic)
{
    bool handled = false;
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    for (size_t i = 0; i < DM_SIGNAL_RUNTIME_MAX; ++i) {
        signal_runtime_entry_t *entry = &s_signal_entries[i];
        if (!entry->in_use) {
            continue;
        }
        if (entry->heartbeat_topic[0] && strcmp(entry->heartbeat_topic, topic) == 0) {
            handled = true;
            dm_signal_action_t action = dm_signal_runtime_handle_tick(&entry->runtime, now_ms);
            handle_signal_audio(entry, action.event);
            apply_signal_mqtt_action(&action);
            if (action.event == DM_SIGNAL_EVENT_COMPLETED) {
                trigger_uid_scenario(entry->device_id, "signal_complete");
            }
        }
    }
    return handled;
}

bool dm_template_runtime_handle_mqtt(const char *topic, const char *payload)
{
    if (!topic) {
        return false;
    }
    bool handled = false;
    handled |= handle_uid_message(topic, payload);
    handled |= handle_signal_message(topic);
    return handled;
}
