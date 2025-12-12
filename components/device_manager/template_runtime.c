#include "dm_template_runtime.h"

#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "dm_runtime_uid.h"
#include "dm_runtime_signal.h"
#include "dm_runtime_mqtt.h"
#include "dm_runtime_flag.h"
#include "dm_runtime_condition.h"
#include "dm_runtime_interval.h"
#include "dm_runtime_sequence.h"
#include "device_manager_utils.h"
#include "audio_player.h"
#include "automation_engine.h"
#include "event_bus.h"
#include "mqtt_core.h"

static const char *TAG = "template_runtime";

typedef struct uid_runtime_entry {
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    dm_uid_runtime_t runtime;
    char topics[DM_UID_TEMPLATE_MAX_SLOTS][DEVICE_MANAGER_TOPIC_MAX_LEN];
    size_t topic_count;
    char start_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char start_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    char broadcast_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char broadcast_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    struct uid_runtime_entry *next;
} uid_runtime_entry_t;

typedef struct signal_runtime_entry {
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    dm_signal_runtime_t runtime;
    char heartbeat_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char reset_topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    bool hold_started;
    bool hold_paused;
    bool hold_active;
    esp_timer_handle_t timeout_timer;
    struct signal_runtime_entry *next;
} signal_runtime_entry_t;

typedef struct mqtt_runtime_entry {
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    dm_mqtt_trigger_runtime_t runtime;
    struct mqtt_runtime_entry *next;
} mqtt_runtime_entry_t;

typedef struct flag_runtime_entry {
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    dm_flag_trigger_runtime_t runtime;
    struct flag_runtime_entry *next;
} flag_runtime_entry_t;

typedef struct condition_runtime_entry {
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    dm_condition_runtime_t runtime;
    struct condition_runtime_entry *next;
} condition_runtime_entry_t;

typedef struct interval_runtime_entry {
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
   dm_interval_task_runtime_t runtime;
    esp_timer_handle_t timer;
    struct interval_runtime_entry *next;
} interval_runtime_entry_t;

typedef struct sequence_runtime_entry {
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    dm_sequence_runtime_t runtime;
    struct sequence_runtime_entry *next;
} sequence_runtime_entry_t;

static uid_runtime_entry_t *s_uid_entries;
static signal_runtime_entry_t *s_signal_entries;
static mqtt_runtime_entry_t *s_mqtt_entries;
static flag_runtime_entry_t *s_flag_entries;
static condition_runtime_entry_t *s_condition_entries;
static interval_runtime_entry_t *s_interval_entries;
static sequence_runtime_entry_t *s_sequence_entries;
static bool s_event_handler_registered = false;

static const char *signal_event_str(dm_signal_event_type_t ev);
static bool signal_debug_enabled(const signal_runtime_entry_t *entry);
static void handle_signal_audio(signal_runtime_entry_t *entry, dm_signal_event_type_t ev);
static void apply_signal_mqtt_action(signal_runtime_entry_t *entry, const dm_signal_action_t *action);
static void signal_timeout_timer_cb(void *arg);
static void restart_signal_timeout_timer(signal_runtime_entry_t *entry);
static void stop_signal_timeout_timer(signal_runtime_entry_t *entry);
static void reset_signal_entry(signal_runtime_entry_t *entry, const char *topic);

static bool payload_to_bool(const char *payload)
{
    if (!payload) {
        return false;
    }
    if (strcasecmp(payload, "true") == 0 || strcasecmp(payload, "on") == 0 ||
        strcasecmp(payload, "yes") == 0) {
        return true;
    }
    if (strcmp(payload, "1") == 0) {
        return true;
    }
    return false;
}

static const char *signal_event_str(dm_signal_event_type_t ev)
{
    switch (ev) {
    case DM_SIGNAL_EVENT_START:
        return "start";
    case DM_SIGNAL_EVENT_CONTINUE:
        return "continue";
    case DM_SIGNAL_EVENT_STOP:
        return "stop";
    case DM_SIGNAL_EVENT_COMPLETED:
        return "completed";
    case DM_SIGNAL_EVENT_NONE:
    default:
        return "none";
    }
}

static bool signal_debug_enabled(const signal_runtime_entry_t *entry)
{
    return entry && entry->runtime.config.debug_logging;
}

static void template_event_handler(const event_bus_message_t *msg)
{
    if (!msg) {
        return;
    }
    switch (msg->type) {
    case EVENT_MQTT_MESSAGE:
        if (!msg->topic[0]) {
            return;
        }
        dm_template_runtime_handle_mqtt(msg->topic, msg->payload[0] ? msg->payload : "");
        break;
    case EVENT_FLAG_CHANGED:
        if (!msg->topic[0]) {
            return;
        }
        dm_template_runtime_handle_flag(msg->topic, payload_to_bool(msg->payload));
        break;
    default:
        break;
    }
}

static void *runtime_alloc(size_t size)
{
    void *ptr = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void free_uid_entries(void)
{
    uid_runtime_entry_t *entry = s_uid_entries;
    while (entry) {
        uid_runtime_entry_t *next = entry->next;
        heap_caps_free(entry);
        entry = next;
    }
    s_uid_entries = NULL;
}

static void free_signal_entries(void)
{
    signal_runtime_entry_t *entry = s_signal_entries;
    while (entry) {
        signal_runtime_entry_t *next = entry->next;
        if (entry->timeout_timer) {
            esp_timer_stop(entry->timeout_timer);
            esp_timer_delete(entry->timeout_timer);
        }
        heap_caps_free(entry);
        entry = next;
    }
    s_signal_entries = NULL;
}

static uint64_t signal_timeout_interval_us(const signal_runtime_entry_t *entry)
{
    if (!entry) {
        return 0;
    }
    uint32_t timeout_ms = entry->runtime.config.heartbeat_timeout_ms
                              ? entry->runtime.config.heartbeat_timeout_ms
                              : 1000;
    return (uint64_t)timeout_ms * 1000ULL;
}

static void restart_signal_timeout_timer(signal_runtime_entry_t *entry)
{
    if (!entry || !entry->timeout_timer) {
        return;
    }
    uint64_t timeout_us = signal_timeout_interval_us(entry);
    if (timeout_us == 0) {
        return;
    }
    esp_timer_stop(entry->timeout_timer);
    esp_timer_start_once(entry->timeout_timer, timeout_us);
}

static void stop_signal_timeout_timer(signal_runtime_entry_t *entry)
{
    if (!entry || !entry->timeout_timer) {
        return;
    }
    esp_timer_stop(entry->timeout_timer);
}

static void signal_timeout_timer_cb(void *arg)
{
    signal_runtime_entry_t *entry = (signal_runtime_entry_t *)arg;
    if (!entry) {
        return;
    }
    dm_signal_action_t action = dm_signal_runtime_handle_timeout(&entry->runtime);
    if (action.event != DM_SIGNAL_EVENT_STOP) {
        return;
    }
    uint32_t timeout_ms = entry->runtime.config.heartbeat_timeout_ms
                              ? entry->runtime.config.heartbeat_timeout_ms
                              : 1000;
    if (signal_debug_enabled(entry)) {
        ESP_LOGW(TAG,
                 "[Signal] dev=%s heartbeat timeout>%ums event=%s acc=%ums",
                 entry->device_id,
                 (unsigned)timeout_ms,
                 signal_event_str(action.event),
                 (unsigned)action.accumulated_ms);
    }
    handle_signal_audio(entry, action.event);
    apply_signal_mqtt_action(entry, &action);
}

static void reset_signal_entry(signal_runtime_entry_t *entry, const char *topic)
{
    if (!entry) {
        return;
    }
    dm_signal_state_reset(&entry->runtime.state);
    entry->hold_started = false;
    entry->hold_paused = false;
    entry->hold_active = false;
    stop_signal_timeout_timer(entry);
    audio_player_stop();
    if (signal_debug_enabled(entry)) {
        ESP_LOGI(TAG,
                 "[Signal] dev=%s reset topic=%s",
                 entry->device_id,
                 topic ? topic : "");
    }
}

static void free_mqtt_entries(void)
{
    mqtt_runtime_entry_t *entry = s_mqtt_entries;
    while (entry) {
        mqtt_runtime_entry_t *next = entry->next;
        heap_caps_free(entry);
        entry = next;
    }
    s_mqtt_entries = NULL;
}

static void free_flag_entries(void)
{
    flag_runtime_entry_t *entry = s_flag_entries;
    while (entry) {
        flag_runtime_entry_t *next = entry->next;
        heap_caps_free(entry);
        entry = next;
    }
    s_flag_entries = NULL;
}

static void free_condition_entries(void)
{
    condition_runtime_entry_t *entry = s_condition_entries;
    while (entry) {
        condition_runtime_entry_t *next = entry->next;
        heap_caps_free(entry);
        entry = next;
    }
    s_condition_entries = NULL;
}

static void free_interval_entries(void)
{
    interval_runtime_entry_t *entry = s_interval_entries;
    while (entry) {
        interval_runtime_entry_t *next = entry->next;
        if (entry->timer) {
            esp_timer_stop(entry->timer);
            esp_timer_delete(entry->timer);
        }
        heap_caps_free(entry);
        entry = next;
    }
    s_interval_entries = NULL;
}

static void free_sequence_entries(void)
{
    sequence_runtime_entry_t *entry = s_sequence_entries;
    while (entry) {
        sequence_runtime_entry_t *next = entry->next;
        heap_caps_free(entry);
        entry = next;
    }
    s_sequence_entries = NULL;
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
    free_uid_entries();
    free_signal_entries();
    free_mqtt_entries();
    free_flag_entries();
    free_condition_entries();
    free_interval_entries();
    free_sequence_entries();
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
    uid_runtime_entry_t *entry = runtime_alloc(sizeof(*entry));
    if (!entry) {
        ESP_LOGE(TAG, "no memory for uid runtime");
        return ESP_ERR_NO_MEM;
    }
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_uid_runtime_init(&entry->runtime, tpl);
    entry->topic_count = tpl->slot_count;
    for (uint8_t i = 0; i < tpl->slot_count && i < DM_UID_TEMPLATE_MAX_SLOTS; ++i) {
        dm_str_copy(entry->topics[i], sizeof(entry->topics[i]), tpl->slots[i].source_id);
    }
    dm_str_copy(entry->start_topic, sizeof(entry->start_topic), tpl->start_topic);
    dm_str_copy(entry->start_payload, sizeof(entry->start_payload), tpl->start_payload);
    dm_str_copy(entry->broadcast_topic, sizeof(entry->broadcast_topic), tpl->broadcast_topic);
    dm_str_copy(entry->broadcast_payload, sizeof(entry->broadcast_payload), tpl->broadcast_payload);
    entry->next = s_uid_entries;
    s_uid_entries = entry;
    ESP_LOGI(TAG, "registered UID runtime for device %s with %zu slots", entry->device_id, entry->topic_count);
    return ESP_OK;
}

static esp_err_t register_signal_runtime(const dm_signal_hold_template_t *tpl, const char *device_id)
{
    if (!tpl || !tpl->heartbeat_topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    signal_runtime_entry_t *entry = runtime_alloc(sizeof(*entry));
    if (!entry) {
        ESP_LOGE(TAG, "no memory for signal runtime");
        return ESP_ERR_NO_MEM;
    }
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_signal_runtime_init(&entry->runtime, tpl);
    dm_str_copy(entry->heartbeat_topic, sizeof(entry->heartbeat_topic), tpl->heartbeat_topic);
    dm_str_copy(entry->reset_topic, sizeof(entry->reset_topic), tpl->reset_topic);
    entry->hold_started = false;
    entry->hold_paused = false;
    entry->hold_active = false;
    entry->timeout_timer = NULL;
    esp_timer_create_args_t args = {
        .callback = signal_timeout_timer_cb,
        .arg = entry,
        .name = "dm_signal_timeout",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_err_t timer_err = esp_timer_create(&args, &entry->timeout_timer);
    if (timer_err != ESP_OK) {
        entry->timeout_timer = NULL;
        ESP_LOGW(TAG, "signal timeout timer create failed for %s: %s",
                 entry->device_id,
                 esp_err_to_name(timer_err));
    }
    entry->next = s_signal_entries;
    s_signal_entries = entry;
    ESP_LOGI(TAG, "registered signal runtime for device %s topic %s", entry->device_id, entry->heartbeat_topic);
    return ESP_OK;
}

static esp_err_t register_mqtt_runtime(const dm_mqtt_trigger_template_t *tpl, const char *device_id)
{
    if (!tpl || tpl->rule_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mqtt_runtime_entry_t *entry = runtime_alloc(sizeof(*entry));
    if (!entry) {
        ESP_LOGE(TAG, "no memory for mqtt trigger runtime");
        return ESP_ERR_NO_MEM;
    }
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_mqtt_trigger_runtime_init(&entry->runtime, tpl);
    entry->next = s_mqtt_entries;
    s_mqtt_entries = entry;
    ESP_LOGI(TAG, "registered MQTT trigger runtime for %s (%u rules)", entry->device_id, tpl->rule_count);
    return ESP_OK;
}

static esp_err_t register_flag_runtime(const dm_flag_trigger_template_t *tpl, const char *device_id)
{
    if (!tpl || tpl->rule_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    flag_runtime_entry_t *entry = runtime_alloc(sizeof(*entry));
    if (!entry) {
        ESP_LOGE(TAG, "no memory for flag trigger runtime");
        return ESP_ERR_NO_MEM;
    }
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_flag_trigger_runtime_init(&entry->runtime, tpl);
    entry->next = s_flag_entries;
    s_flag_entries = entry;
    ESP_LOGI(TAG, "registered flag trigger runtime for %s (%u rules)", entry->device_id, tpl->rule_count);
    return ESP_OK;
}

static esp_err_t register_condition_runtime(const dm_condition_template_t *tpl, const char *device_id)
{
    if (!tpl || tpl->rule_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    condition_runtime_entry_t *entry = runtime_alloc(sizeof(*entry));
    if (!entry) {
        ESP_LOGE(TAG, "no memory for condition runtime");
        return ESP_ERR_NO_MEM;
    }
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_condition_runtime_init(&entry->runtime, tpl);
    entry->next = s_condition_entries;
    s_condition_entries = entry;
    ESP_LOGI(TAG, "registered condition runtime for %s (%u rules)", entry->device_id, tpl->rule_count);
    return ESP_OK;
}

static void interval_timer_callback(void *arg)
{
    interval_runtime_entry_t *entry = (interval_runtime_entry_t *)arg;
    if (!entry) {
        return;
    }
    const char *scenario = entry->runtime.config.scenario;
    if (!scenario[0]) {
        return;
    }
    esp_err_t err = automation_engine_trigger(entry->device_id, scenario);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "interval trigger %s/%s failed: %s",
                 entry->device_id,
                 scenario,
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[Interval] dev=%s scenario=%s", entry->device_id, scenario);
    }
}

static esp_err_t register_interval_runtime(const dm_interval_task_template_t *tpl, const char *device_id)
{
    if (!tpl || tpl->interval_ms == 0 || !tpl->scenario[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    interval_runtime_entry_t *entry = runtime_alloc(sizeof(*entry));
    if (!entry) {
        ESP_LOGE(TAG, "no memory for interval runtime");
        return ESP_ERR_NO_MEM;
    }
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_interval_task_runtime_init(&entry->runtime, tpl);
    esp_timer_create_args_t args = {
        .callback = interval_timer_callback,
        .arg = entry,
        .name = "dm_interval",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_err_t err = esp_timer_create(&args, &entry->timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interval timer create failed: %s", esp_err_to_name(err));
        heap_caps_free(entry);
        return err;
    }
    uint64_t interval_us = (uint64_t)entry->runtime.config.interval_ms * 1000ULL;
    if (interval_us < 1000ULL) {
        interval_us = 1000ULL;
    }
    err = esp_timer_start_periodic(entry->timer, interval_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interval timer start failed: %s", esp_err_to_name(err));
        esp_timer_delete(entry->timer);
        heap_caps_free(entry);
        return err;
    }
    entry->next = s_interval_entries;
    s_interval_entries = entry;
    ESP_LOGI(TAG, "registered interval runtime for %s every %u ms",
             entry->device_id,
             (unsigned)entry->runtime.config.interval_ms);
    return ESP_OK;
}

static esp_err_t register_sequence_runtime(const dm_sequence_template_t *tpl, const char *device_id)
{
    if (!tpl || tpl->step_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    sequence_runtime_entry_t *entry = runtime_alloc(sizeof(*entry));
    if (!entry) {
        ESP_LOGE(TAG, "no memory for sequence runtime");
        return ESP_ERR_NO_MEM;
    }
    dm_str_copy(entry->device_id, sizeof(entry->device_id), device_id);
    dm_sequence_runtime_init(&entry->runtime, tpl);
    entry->next = s_sequence_entries;
    s_sequence_entries = entry;
    ESP_LOGI(TAG, "registered sequence runtime for %s (%u steps)",
             entry->device_id,
             (unsigned)tpl->step_count);
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
    case DM_TEMPLATE_TYPE_MQTT_TRIGGER:
        return register_mqtt_runtime(&tpl->data.mqtt, device_id);
    case DM_TEMPLATE_TYPE_FLAG_TRIGGER:
        return register_flag_runtime(&tpl->data.flag, device_id);
    case DM_TEMPLATE_TYPE_IF_CONDITION:
        return register_condition_runtime(&tpl->data.condition, device_id);
    case DM_TEMPLATE_TYPE_INTERVAL_TASK:
        return register_interval_runtime(&tpl->data.interval, device_id);
    case DM_TEMPLATE_TYPE_SEQUENCE_LOCK:
        return register_sequence_runtime(&tpl->data.sequence, device_id);
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
    for (uid_runtime_entry_t *entry = s_uid_entries; entry; entry = entry->next) {
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
        return;
    }
    // Forward local publications back through the template runtime so MQTT triggers
    // and other template listeners react to internally generated events.
    dm_template_runtime_handle_mqtt(topic, body);
}

static bool payload_matches(const char *expected, const char *actual)
{
    if (!expected || !expected[0]) {
        return true;
    }
    if (!actual) {
        actual = "";
    }
    return strcmp(expected, actual) == 0;
}

static void reset_uid_entry(uid_runtime_entry_t *entry)
{
    if (!entry) {
        return;
    }
    dm_uid_runtime_reset(&entry->runtime);
}

static bool handle_uid_start_event(uid_runtime_entry_t *entry, const char *topic, const char *payload)
{
    if (!entry || !entry->start_topic[0] || !topic) {
        return false;
    }
    if (strcmp(entry->start_topic, topic) != 0) {
        return false;
    }
    if (!payload_matches(entry->start_payload, payload)) {
        return false;
    }
    ESP_LOGI(TAG,
             "[UID] dev=%s start topic=%s payload='%s'",
             entry->device_id,
             topic,
             payload ? payload : "");
    reset_uid_entry(entry);
    if (entry->broadcast_topic[0]) {
        publish_mqtt_payload(entry->broadcast_topic, entry->broadcast_payload);
    }
    return true;
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

static void trigger_device_scenario(const char *device_id, const char *scenario_id)
{
    if (!scenario_id || !scenario_id[0]) {
        return;
    }
    esp_err_t err = automation_engine_trigger(device_id, scenario_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scenario %s/%s failed: %s", device_id, scenario_id, esp_err_to_name(err));
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
    const char *body = payload ? payload : "";
    for (uid_runtime_entry_t *entry = s_uid_entries; entry; entry = entry->next) {
        if (handle_uid_start_event(entry, topic, body)) {
            handled = true;
            continue;
        }
        for (size_t t = 0; t < entry->topic_count; ++t) {
            if (entry->topics[t][0] && strcmp(entry->topics[t], topic) == 0) {
                handled = true;
                dm_uid_action_t action = dm_uid_runtime_handle_value(&entry->runtime, topic, body);
                ESP_LOGI(TAG, "[UID] dev=%s topic=%s event=%s payload='%s'",
                         entry->device_id,
                         topic,
                         uid_event_str(action.event),
                         body);
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
            if (audio_player_play(cfg->hold_track) == ESP_OK) {
                if (signal_debug_enabled(entry)) {
                    ESP_LOGI(TAG, "[Signal] dev=%s hold track play %s", entry->device_id, cfg->hold_track);
                }
            } else {
                if (signal_debug_enabled(entry)) {
                    ESP_LOGW(TAG, "[Signal] dev=%s failed to play hold track %s", entry->device_id, cfg->hold_track);
                }
            }
            entry->hold_started = true;
            entry->hold_paused = false;
            entry->hold_active = true;
        } else if (entry->hold_paused) {
            audio_player_resume();
            if (signal_debug_enabled(entry)) {
                ESP_LOGI(TAG, "[Signal] dev=%s hold track resume %s", entry->device_id, cfg->hold_track);
            }
            entry->hold_paused = false;
            entry->hold_active = true;
        }
        break;
    case DM_SIGNAL_EVENT_STOP:
        if (entry->hold_active) {
            audio_player_pause();
            if (signal_debug_enabled(entry)) {
                ESP_LOGI(TAG, "[Signal] dev=%s hold track pause %s", entry->device_id, cfg->hold_track);
            }
            entry->hold_paused = true;
            entry->hold_active = false;
        }
        break;
    case DM_SIGNAL_EVENT_COMPLETED:
        if (entry->hold_active || entry->hold_paused) {
            audio_player_stop();
            if (signal_debug_enabled(entry)) {
                ESP_LOGI(TAG, "[Signal] dev=%s hold track stop %s", entry->device_id, cfg->hold_track);
            }
        }
        entry->hold_started = false;
        entry->hold_paused = false;
        entry->hold_active = false;
        if (cfg->complete_track[0]) {
            if (audio_player_play(cfg->complete_track) == ESP_OK) {
                if (signal_debug_enabled(entry)) {
                    ESP_LOGI(TAG, "[Signal] dev=%s complete track %s", entry->device_id, cfg->complete_track);
                }
            } else {
                if (signal_debug_enabled(entry)) {
                    ESP_LOGW(TAG, "[Signal] dev=%s failed to play complete track %s", entry->device_id, cfg->complete_track);
                }
            }
        }
        break;
    default:
        break;
    }
}

static void apply_signal_mqtt_action(signal_runtime_entry_t *entry, const dm_signal_action_t *action)
{
    if (!action) {
        return;
    }
    if (action->signal_on) {
        publish_mqtt_payload(action->signal_topic, action->signal_payload_on);
        if (signal_debug_enabled(entry)) {
            ESP_LOGI(TAG, "[Signal] dev=%s publish %s payload='%s'",
                     entry ? entry->device_id : "",
                     action->signal_topic,
                     action->signal_payload_on ? action->signal_payload_on : "");
        }
    }
    if (action->signal_off) {
        publish_mqtt_payload(action->signal_topic, action->signal_payload_off);
        if (signal_debug_enabled(entry)) {
            ESP_LOGI(TAG, "[Signal] dev=%s publish %s payload='%s'",
                     entry ? entry->device_id : "",
                     action->signal_topic,
                     action->signal_payload_off ? action->signal_payload_off : "");
        }
    }
}

static void apply_sequence_step_hint(const dm_sequence_step_t *step)
{
    if (!step) {
        return;
    }
    if (step->hint_topic[0]) {
        publish_mqtt_payload(step->hint_topic, step->hint_payload);
    }
    if (step->hint_audio_track[0]) {
        audio_player_play(step->hint_audio_track);
    }
}

static void apply_sequence_success(sequence_runtime_entry_t *entry)
{
    if (!entry) {
        return;
    }
    const dm_sequence_template_t *cfg = &entry->runtime.config;
    publish_mqtt_payload(cfg->success_topic, cfg->success_payload);
    if (cfg->success_audio_track[0]) {
        audio_player_play(cfg->success_audio_track);
    }
    trigger_device_scenario(entry->device_id, cfg->success_scenario);
}

static void apply_sequence_fail(sequence_runtime_entry_t *entry)
{
    if (!entry) {
        return;
    }
    const dm_sequence_template_t *cfg = &entry->runtime.config;
    publish_mqtt_payload(cfg->fail_topic, cfg->fail_payload);
    if (cfg->fail_audio_track[0]) {
        audio_player_play(cfg->fail_audio_track);
    }
    trigger_device_scenario(entry->device_id, cfg->fail_scenario);
}

static bool handle_sequence_message(const char *topic, const char *payload)
{
    if (!topic || !topic[0]) {
        return false;
    }
    bool handled = false;
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    for (sequence_runtime_entry_t *entry = s_sequence_entries; entry; entry = entry->next) {
        dm_sequence_action_t action =
            dm_sequence_runtime_handle(&entry->runtime, topic, payload, now_ms);
        if (action.type == DM_SEQUENCE_EVENT_NONE && !action.step) {
            continue;
        }
        handled = true;
        const char *step_topic = action.step && action.step->topic[0] ? action.step->topic : topic;
        switch (action.type) {
        case DM_SEQUENCE_EVENT_STEP_OK:
            ESP_LOGI(TAG, "[Sequence] dev=%s step ok topic=%s payload='%s'",
                     entry->device_id,
                     step_topic,
                     payload ? payload : "");
            if (action.step) {
                apply_sequence_step_hint(action.step);
            }
            break;
        case DM_SEQUENCE_EVENT_COMPLETED:
            ESP_LOGI(TAG, "[Sequence] dev=%s completed topic=%s payload='%s'",
                     entry->device_id,
                     step_topic,
                     payload ? payload : "");
            if (action.step) {
                apply_sequence_step_hint(action.step);
            }
            apply_sequence_success(entry);
            break;
        case DM_SEQUENCE_EVENT_FAILED:
            ESP_LOGW(TAG, "[Sequence] dev=%s failed topic=%s payload='%s'%s",
                     entry->device_id,
                     step_topic,
                     payload ? payload : "",
                     action.timeout ? " (timeout)" : "");
            apply_sequence_fail(entry);
            break;
        case DM_SEQUENCE_EVENT_NONE:
        default:
            ESP_LOGD(TAG, "[Sequence] dev=%s ignored topic=%s payload='%s'",
                     entry->device_id,
                     step_topic,
                     payload ? payload : "");
            break;
        }
    }
    return handled;
}

static bool handle_signal_message(const char *topic, const char *payload)
{
    bool handled = false;
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    for (signal_runtime_entry_t *entry = s_signal_entries; entry; entry = entry->next) {
        if (entry->reset_topic[0] && strcmp(entry->reset_topic, topic) == 0) {
            handled = true;
            reset_signal_entry(entry, topic);
            continue;
        }
        if (entry->heartbeat_topic[0] && strcmp(entry->heartbeat_topic, topic) == 0) {
            handled = true;
            dm_signal_action_t action = dm_signal_runtime_handle_tick(&entry->runtime, now_ms);
            if (action.event == DM_SIGNAL_EVENT_COMPLETED || action.event == DM_SIGNAL_EVENT_STOP) {
                stop_signal_timeout_timer(entry);
            } else if (action.event != DM_SIGNAL_EVENT_NONE) {
                restart_signal_timeout_timer(entry);
            }
            if (signal_debug_enabled(entry)) {
                ESP_LOGI(TAG,
                         "[Signal] dev=%s heartbeat topic=%s payload='%s' event=%s acc=%ums",
                         entry->device_id,
                         topic,
                         payload ? payload : "",
                         signal_event_str(action.event),
                         (unsigned)action.accumulated_ms);
            }
            handle_signal_audio(entry, action.event);
            apply_signal_mqtt_action(entry, &action);
            if (action.event == DM_SIGNAL_EVENT_COMPLETED) {
                trigger_uid_scenario(entry->device_id, "signal_complete");
            }
        }
    }
    return handled;
}

static bool handle_mqtt_trigger_message(const char *topic, const char *payload)
{
    bool handled = false;
    for (mqtt_runtime_entry_t *entry = s_mqtt_entries; entry; entry = entry->next) {
        const dm_mqtt_trigger_rule_t *rule =
            dm_mqtt_trigger_runtime_match(&entry->runtime, topic, payload);
        if (!rule) {
            ESP_LOGD(TAG, "[MQTT trigger] dev=%s no match topic=%s payload='%s'",
                     entry->device_id,
                     topic,
                     payload ? payload : "");
            continue;
        }
        handled = true;
        ESP_LOGI(TAG, "[MQTT trigger] dev=%s topic=%s scenario=%s payload='%s'",
                 entry->device_id,
                 topic,
                 rule->scenario,
                 payload ? payload : "");
        esp_err_t err = automation_engine_trigger(entry->device_id, rule->scenario);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scenario %s/%s failed: %s",
                     entry->device_id,
                     rule->scenario,
                     esp_err_to_name(err));
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
    handled |= handle_signal_message(topic, payload);
    handled |= handle_mqtt_trigger_message(topic, payload);
    handled |= handle_sequence_message(topic, payload);
    return handled;
}

bool dm_template_runtime_handle_flag(const char *flag_name, bool state)
{
    if (!flag_name) {
        return false;
    }
    bool handled = false;
    for (flag_runtime_entry_t *entry = s_flag_entries; entry; entry = entry->next) {
        const dm_flag_trigger_rule_t *rule =
            dm_flag_trigger_runtime_handle(&entry->runtime, flag_name, state);
        if (!rule) {
            continue;
        }
        handled = true;
        ESP_LOGI(TAG, "[Flag trigger] dev=%s flag=%s state=%d scenario=%s",
                 entry->device_id,
                 rule->flag,
                 (int)state,
                 rule->scenario);
        esp_err_t err = automation_engine_trigger(entry->device_id, rule->scenario);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scenario %s/%s failed: %s",
                     entry->device_id,
                     rule->scenario,
                     esp_err_to_name(err));
        }
    }
    for (condition_runtime_entry_t *entry = s_condition_entries; entry; entry = entry->next) {
        bool changed = false;
        bool result = false;
        if (dm_condition_runtime_handle_flag(&entry->runtime, flag_name, state, &changed, &result)) {
            handled = true;
            if (!changed) {
                continue;
            }
            const char *scenario = result ? entry->runtime.config.true_scenario
                                          : entry->runtime.config.false_scenario;
            if (scenario[0]) {
                esp_err_t err = automation_engine_trigger(entry->device_id, scenario);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "if_condition trigger %s/%s failed: %s",
                             entry->device_id,
                             scenario,
                             esp_err_to_name(err));
                }
            }
        }
    }
    return handled;
}
