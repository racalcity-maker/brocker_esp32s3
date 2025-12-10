#include "device_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <limits.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "event_bus.h"
#include "esp_task_wdt.h"

#include "dm_profiles.h"
#include "dm_storage.h"
#include "device_manager_utils.h"
#include "dm_template_runtime.h"

static const char *TAG = "device_manager";
static const uint32_t DEVICE_CONFIG_VERSION = 1;
static const char *CONFIG_BACKUP_PATH = "/sdcard/brocker_devices.json";

#define DM_DEVICE_MAX DEVICE_MANAGER_MAX_DEVICES
#define DM_SCENARIO_MAX DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE

static SemaphoreHandle_t s_lock;
static device_manager_config_t *s_config = NULL;
static device_manager_config_t *s_work = NULL;
static bool s_config_ready = false;
static bool s_cjson_hooks_set = false;

static void *dm_cjson_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void dm_cjson_free(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

static void dm_init_cjson_hooks(void)
{
    if (s_cjson_hooks_set) {
        return;
    }
    cJSON_Hooks hooks = {
        .malloc_fn = dm_cjson_malloc,
        .free_fn = dm_cjson_free,
    };
    cJSON_InitHooks(&hooks);
    s_cjson_hooks_set = true;
}

static void register_templates_from_config(device_manager_config_t *cfg);




static bool populate_config_from_json(device_manager_config_t *cfg, const cJSON *root);

#if CONFIG_ESP_TASK_WDT
static inline void feed_wdt(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}
#else
static inline void feed_wdt(void) {}
#endif

static void dm_copy(void *dest, const void *src, size_t len)
{
    if (!dest || !src || len == 0) {
        return;
    }
    ESP_LOGI(TAG, "dm_copy size=%zu, psram_free=%u, internal_free=%u",
             len,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    const size_t chunk = 512;
    size_t offset = 0;
    while (offset < len) {
        size_t part = chunk;
        if (part > len - offset) {
            part = len - offset;
        }
        memcpy(d + offset, s + offset, part);
        offset += part;
        feed_wdt();
    }
}

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

static const char *condition_to_string(device_condition_type_t cond)
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

static bool condition_from_string(const char *name, device_condition_type_t *out)
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

static const char *tab_type_to_string(device_tab_type_t type)
{
    for (size_t i = 0; i < sizeof(k_tab_types) / sizeof(k_tab_types[0]); ++i) {
        if (k_tab_types[i].type == type) {
            return k_tab_types[i].name;
        }
    }
    return "custom";
}

static bool tab_type_from_string(const char *name, device_tab_type_t *out)
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

static const char *action_type_to_string(device_action_type_t type)
{
    for (size_t i = 0; i < sizeof(k_action_types) / sizeof(k_action_types[0]); ++i) {
        if (k_action_types[i].type == type) {
            return k_action_types[i].name;
        }
    }
    return "nop";
}

static bool action_type_from_string(const char *name, device_action_type_t *out)
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

static void dm_lock(void)
{
    if (s_lock) {
        while (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            feed_wdt();
            vTaskDelay(1);
        }
    }
}

static void dm_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void register_templates_from_config(device_manager_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    dm_template_runtime_reset();
    for (uint8_t i = 0; i < cfg->device_count && i < DEVICE_MANAGER_MAX_DEVICES; ++i) {
        device_descriptor_t *dev = &cfg->devices[i];
        if (dev->template_assigned) {
            esp_err_t err = dm_template_runtime_register(&dev->template_config, dev->id);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "template runtime register failed for %s: %s", dev->id, esp_err_to_name(err));
            }
        }
    }
}

static void template_to_json_string(cJSON *obj, const char *key, const char *value)
{
    if (obj && key && value && value[0]) {
        cJSON_AddStringToObject(obj, key, value);
    }
}

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
    cJSON_AddNumberToObject(root, "required_hold_ms", (double)tpl->required_hold_ms);
    cJSON_AddNumberToObject(root, "heartbeat_timeout_ms", (double)tpl->heartbeat_timeout_ms);
    template_to_json_string(root, "hold_track", tpl->hold_track);
    cJSON_AddBoolToObject(root, "hold_track_loop", tpl->hold_track_loop);
    template_to_json_string(root, "complete_track", tpl->complete_track);
    return root;
}

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
    default:
        break;
    }
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

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
    default:
        ok = false;
        break;
    }
    if (!ok) {
        dev->template_assigned = false;
    }
    return dev->template_assigned;
}

static void load_defaults(device_manager_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->schema_version = DEVICE_CONFIG_VERSION;
    cfg->generation = 1;
    cfg->tab_limit = DEVICE_MANAGER_MAX_TABS;
    cfg->profile_count = 0;
    cfg->active_profile[0] = 0;
    dm_profiles_ensure_active(cfg);
}

esp_err_t dm_storage_internal_parse(const char *json, size_t len, device_manager_config_t *cfg)
{
    if (!json || len == 0 || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    bool ok = populate_config_from_json(cfg, root);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}



esp_err_t device_manager_init(void)
{
    ESP_LOGI(TAG, ">>> ENTER device_manager_init()");
    ESP_LOGI(TAG, "PSRAM free: %u, internal free: %u",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "device_manager_init start");
    dm_init_cjson_hooks();
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); }
    if (s_config_ready) {
        ESP_LOGI(TAG, "device_manager already initialized");
        return ESP_OK;
    }
    if (!s_config) {
        size_t total = sizeof(device_manager_config_t);
        ESP_LOGI(TAG, "allocating config buffer (%zu bytes)", total);
        s_config = heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_config) {
            s_config = calloc(1, total);
        }
        ESP_RETURN_ON_FALSE(s_config != NULL, ESP_ERR_NO_MEM, TAG, "alloc config failed");
    }
    ESP_LOGI(TAG, "loading defaults to config buffer");
    load_defaults(s_config);
    feed_wdt();
    if (!s_work) {
        size_t total = sizeof(device_manager_config_t);
        ESP_LOGI(TAG, "allocating staging buffer (%zu bytes)", total);
        s_work = heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_work) {
            s_work = calloc(1, total);
        }
        if (!s_work) {
            ESP_LOGW(TAG, "no memory for staging buffer, will allocate on demand");
        }
    }

    device_manager_config_t *temp = heap_caps_calloc(1, sizeof(*temp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!temp) {
        ESP_LOGE(TAG, "no memory for temp config");
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    ESP_LOGI(TAG, "Expected cfg size=%zu", sizeof(*temp));
    ESP_LOGI(TAG, "loading config from %s", CONFIG_BACKUP_PATH);
    esp_err_t load_err = dm_storage_load(CONFIG_BACKUP_PATH, temp);
    feed_wdt();
    if (load_err == ESP_OK) {
        dm_lock();
        feed_wdt();
        dm_copy(s_config, temp, sizeof(*temp));
        s_config->generation++;
        dm_unlock();
        ESP_LOGI(TAG, "device config loaded from file");
    } else {
        dm_lock();
        load_defaults(s_config);
        feed_wdt();
        ESP_LOGW(TAG, "using defaults, saving to file: %s", esp_err_to_name(load_err));
        ESP_ERROR_CHECK_WITHOUT_ABORT(dm_storage_save(CONFIG_BACKUP_PATH, s_config));
        dm_unlock();
    }
    free(temp);
    dm_profiles_sync_from_active(s_config, true);
    dm_profiles_sync_to_active(s_config);
    s_config_ready = true;
    esp_err_t rt_err = dm_template_runtime_init();
    if (rt_err != ESP_OK) {
        ESP_LOGE(TAG, "template runtime init failed: %s", esp_err_to_name(rt_err));
        return rt_err;
    }
    register_templates_from_config(s_config);
    ESP_LOGI(TAG, "device_manager_init finished successfully");
    for (int i = 0; i < 10; ++i) {
        feed_wdt();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "device_manager_init done");
    return ESP_OK;
}

const device_manager_config_t *device_manager_get(void)
{
    return s_config;
}

esp_err_t device_manager_reload_from_nvs(void)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    device_manager_config_t *temp = heap_caps_calloc(1, sizeof(*temp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!temp) {
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    esp_err_t err = dm_storage_load(CONFIG_BACKUP_PATH, temp);
    feed_wdt();
    if (err != ESP_OK) {
        free(temp);
        return err;
    }
    dm_lock();
    feed_wdt();
    dm_copy(s_config, temp, sizeof(*temp));
    s_config->generation++;
    dm_profiles_sync_from_active(s_config, true);
    dm_profiles_sync_to_active(s_config);
    feed_wdt();
    dm_unlock();
    free(temp);
    register_templates_from_config(s_config);
    return ESP_OK;
}

static esp_err_t persist_locked(void)
{
    device_manager_config_t *snapshot = s_work;
    bool temp_alloc = false;
    if (!snapshot) {
        snapshot = heap_caps_calloc(1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!snapshot) {
            snapshot = calloc(1, sizeof(*snapshot));
        }
        temp_alloc = true;
    }
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    dm_profiles_sync_to_active(s_config);
    esp_err_t profile_err = dm_profiles_store_active(s_config);
    if (profile_err != ESP_OK) {
        if (temp_alloc) {
            free(snapshot);
        } else {
            memset(snapshot, 0, sizeof(*snapshot));
        }
        return profile_err;
    }
    dm_copy(snapshot, s_config, sizeof(*snapshot));
    feed_wdt();
    esp_err_t err = dm_storage_save(CONFIG_BACKUP_PATH, snapshot);
    if (temp_alloc) {
        free(snapshot);
    } else {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return err;
}

esp_err_t device_manager_save_snapshot(void)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    dm_lock();
    esp_err_t err = persist_locked();
    dm_unlock();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "device config saved to file");
    }
    return err;
}

esp_err_t device_manager_apply(const device_manager_config_t *next)
{
    if (!next || !s_config_ready) {
        return ESP_ERR_INVALID_ARG;
    }
    dm_lock();
    feed_wdt();
    vTaskDelay(1);
    dm_copy(s_config, next, sizeof(*next));
    feed_wdt();
    s_config->generation++;
    dm_profiles_sync_to_active(s_config);
    feed_wdt();
    esp_err_t err = persist_locked();
    dm_unlock();
    if (err == ESP_OK) {
        event_bus_message_t msg = {
            .type = EVENT_DEVICE_CONFIG_CHANGED,
        };
        event_bus_post(&msg, 0);
    }
    return err;
}

esp_err_t device_manager_sync_file(void)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    device_manager_config_t *snapshot = s_work;
    bool temp_alloc = false;
    if (!snapshot) {
        snapshot = heap_caps_calloc(1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!snapshot) {
            snapshot = calloc(1, sizeof(*snapshot));
        }
        temp_alloc = true;
    }
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    dm_lock();
    dm_profiles_sync_to_active(s_config);
    esp_err_t profile_err = dm_profiles_store_active(s_config);
    if (profile_err != ESP_OK) {
        dm_unlock();
        if (temp_alloc) {
            free(snapshot);
        } else {
            memset(snapshot, 0, sizeof(*snapshot));
        }
        return profile_err;
    }
    dm_copy(snapshot, s_config, sizeof(*snapshot));
    dm_unlock();
    esp_err_t err = dm_storage_save(CONFIG_BACKUP_PATH, snapshot);
    if (temp_alloc) {
        free(snapshot);
    } else {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return err;
}

static cJSON *step_to_json(const device_action_step_t *step)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "type", action_type_to_string(step->type));
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
        cJSON_AddStringToObject(wait, "mode", condition_to_string(step->data.wait_flags.mode));
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
    if (!action_type_from_string(type_item->valuestring, &type)) {
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
        if (!mode || !cJSON_IsString(mode) || !condition_from_string(mode->valuestring, &step->data.wait_flags.mode)) {
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
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema", cfg->schema_version);
    cJSON_AddNumberToObject(root, "generation", cfg->generation);
    cJSON_AddNumberToObject(root, "tab_limit", cfg->tab_limit);
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
    for (uint8_t i = 0; i < cfg->device_count && i < DEVICE_MANAGER_MAX_DEVICES; ++i) {
        feed_wdt();
        const device_descriptor_t *dev = &cfg->devices[i];
        cJSON *d = cJSON_CreateObject();
        if (!d) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddItemToArray(devices, d);
        cJSON_AddStringToObject(d, "id", dev->id);
        cJSON_AddStringToObject(d, "name", dev->display_name);

        cJSON *tabs = cJSON_AddArrayToObject(d, "tabs");
        if (!tabs) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        for (uint8_t t = 0; t < dev->tab_count && t < DEVICE_MANAGER_MAX_TABS; ++t) {
            const device_tab_t *tab = &dev->tabs[t];
            cJSON *tab_obj = cJSON_CreateObject();
            if (!tab_obj) {
                err = ESP_ERR_NO_MEM;
                goto cleanup;
            }
            cJSON_AddItemToArray(tabs, tab_obj);
            cJSON_AddStringToObject(tab_obj, "type", tab_type_to_string(tab->type));
            cJSON_AddStringToObject(tab_obj, "label", tab->label);
            cJSON_AddStringToObject(tab_obj, "extra", tab->extra_payload);
            feed_wdt();
        }

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
    return err;
}

esp_err_t device_manager_export_json(char **out_json, size_t *out_len)
{
    return device_manager_export_profile_json(NULL, out_json, out_len);
}

esp_err_t device_manager_export_profile_json(const char *profile_id, char **out_json, size_t *out_len)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_config) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_json = NULL;
    if (out_len) {
        *out_len = 0;
    }
    device_manager_config_t *snapshot = s_work;
    bool temp_alloc = false;
    if (!snapshot) {
        snapshot = heap_caps_calloc(1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!snapshot) {
            snapshot = calloc(1, sizeof(*snapshot));
        }
        temp_alloc = true;
    }
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    dm_lock();
    feed_wdt();
    dm_copy(snapshot, s_config, sizeof(*snapshot));
    feed_wdt();
    dm_unlock();
    if (profile_id && profile_id[0]) {
        dm_str_copy(snapshot->active_profile, sizeof(snapshot->active_profile), profile_id);
    }
    dm_profiles_ensure_active(snapshot);
    dm_profiles_sync_from_active(snapshot, false);
    esp_err_t err = dm_storage_export_json(snapshot, out_json, out_len);
    if (temp_alloc) {
        free(snapshot);
    } else {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return err;
}

static bool populate_config_from_json(device_manager_config_t *cfg, const cJSON *root)
{
    if (!cfg || !root) {
        return false;
    }
    load_defaults(cfg);
    cfg->schema_version = json_number_to_u32(cJSON_GetObjectItem(root, "schema"), DEVICE_CONFIG_VERSION);
    cfg->generation = json_number_to_u32(cJSON_GetObjectItem(root, "generation"), cfg->generation);
    uint32_t tab_limit = json_number_to_u32(cJSON_GetObjectItem(root, "tab_limit"), DEVICE_MANAGER_MAX_TABS);
    cfg->tab_limit = (uint8_t)((tab_limit > DEVICE_MANAGER_MAX_TABS) ? DEVICE_MANAGER_MAX_TABS : tab_limit);
    cfg->profile_count = 0;
    cfg->active_profile[0] = 0;
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
                profile->device_count = (cnt > DEVICE_MANAGER_MAX_DEVICES) ? DEVICE_MANAGER_MAX_DEVICES : (uint8_t)cnt;
            }
        }
    }
    const cJSON *active = cJSON_GetObjectItem(root, "active_profile");
    if (cJSON_IsString(active) && active->valuestring && active->valuestring[0]) {
        dm_str_copy(cfg->active_profile, sizeof(cfg->active_profile), active->valuestring);
    }
    dm_profiles_ensure_active(cfg);

    const cJSON *devices = cJSON_GetObjectItem(root, "devices");
    if (!devices || !cJSON_IsArray(devices)) {
        cfg->device_count = 0;
        return true;
    }
    uint8_t dev_count = 0;
    const cJSON *dev_node = NULL;
    cJSON_ArrayForEach(dev_node, devices) {
        if (dev_count >= DEVICE_MANAGER_MAX_DEVICES) {
            break;
        }
        if (!cJSON_IsObject(dev_node)) {
            continue;
        }
        device_descriptor_t *dev = &cfg->devices[dev_count];
        memset(dev, 0, sizeof(*dev));
        dm_str_copy(dev->id, sizeof(dev->id), cJSON_GetStringValue(cJSON_GetObjectItem(dev_node, "id")));
        dm_str_copy(dev->display_name, sizeof(dev->display_name),
                 cJSON_GetStringValue(cJSON_GetObjectItem(dev_node, "name")));
        feed_wdt();
        const cJSON *tabs = cJSON_GetObjectItem(dev_node, "tabs");
        uint8_t tab_count = 0;
        if (cJSON_IsArray(tabs)) {
            const cJSON *tab_node = NULL;
            cJSON_ArrayForEach(tab_node, tabs) {
                if (tab_count >= DEVICE_MANAGER_MAX_TABS) {
                    break;
                }
                if (!cJSON_IsObject(tab_node)) {
                    continue;
                }
                const cJSON *type_item = cJSON_GetObjectItem(tab_node, "type");
                if (!cJSON_IsString(type_item)) {
                    continue;
                }
                device_tab_type_t tab_type;
                if (!tab_type_from_string(type_item->valuestring, &tab_type)) {
                    continue;
                }
                device_tab_t *tab = &dev->tabs[tab_count++];
                tab->type = tab_type;
                dm_str_copy(tab->label, sizeof(tab->label),
                         cJSON_GetStringValue(cJSON_GetObjectItem(tab_node, "label")));
                dm_str_copy(tab->extra_payload, sizeof(tab->extra_payload),
                         cJSON_GetStringValue(cJSON_GetObjectItem(tab_node, "extra")));
                feed_wdt();
            }
        }
        dev->tab_count = tab_count;

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

esp_err_t device_manager_apply_profile_json(const char *profile_id, const char *json, size_t len)
{
    if (!json || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    device_manager_config_t *next = s_work;
    bool temp_alloc = false;
    if (!next) {
        next = heap_caps_calloc(1, sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) {
            next = calloc(1, sizeof(*next));
        }
        temp_alloc = true;
    }
    if (!next) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    bool ok = populate_config_from_json(next, root);
    cJSON_Delete(root);
    if (!ok) {
        if (temp_alloc) {
            free(next);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (profile_id && profile_id[0]) {
        dm_str_copy(next->active_profile, sizeof(next->active_profile), profile_id);
    }
    feed_wdt();
    esp_err_t err = device_manager_apply(next);
    if (temp_alloc) {
        free(next);
    } else {
        memset(next, 0, sizeof(*next));
    }
    return err;
}

esp_err_t device_manager_profile_create(const char *id, const char *name, const char *clone_id)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dm_profiles_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    dm_lock();
    dm_profiles_ensure_active(s_config);
    if (dm_profiles_find_by_id(s_config, id)) {
        dm_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config->profile_count >= DEVICE_MANAGER_MAX_PROFILES) {
        dm_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (clone_id && clone_id[0]) {
        const device_manager_profile_t *clone_profile = dm_profiles_find_by_id(s_config, clone_id);
        if (clone_profile) {
            uint8_t cloned_count = 0;
            esp_err_t clone_err = dm_profiles_load_profile(clone_profile->id, s_config->devices, &cloned_count);
            if (clone_err != ESP_OK) {
                dm_unlock();
                return clone_err;
            }
            s_config->device_count = cloned_count;
        } else {
            ESP_LOGW(TAG, "clone profile %s not found, using active", clone_id);
        }
    }
    device_manager_profile_t *dst = &s_config->profiles[s_config->profile_count++];
    memset(dst, 0, sizeof(*dst));
    dm_str_copy(dst->id, sizeof(dst->id), id);
    dm_str_copy(dst->name, sizeof(dst->name), (name && name[0]) ? name : id);
    dm_str_copy(s_config->active_profile, sizeof(s_config->active_profile), dst->id);
    dm_profiles_sync_to_active(s_config);
    esp_err_t store_err = dm_profiles_store_active(s_config);
    if (store_err != ESP_OK) {
        s_config->profile_count--;
        dm_unlock();
        return store_err;
    }
    s_config->generation++;
    esp_err_t err = persist_locked();
    dm_unlock();
    return err;
}

esp_err_t device_manager_profile_delete(const char *id)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    dm_lock();
    dm_profiles_ensure_active(s_config);
    if (s_config->profile_count <= 1) {
        dm_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    int idx = -1;
    for (uint8_t i = 0; i < s_config->profile_count; ++i) {
        if (strcasecmp(s_config->profiles[i].id, id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        dm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t delete_err = dm_profiles_delete_profile_file(id);
    if (delete_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to remove profile %s file: %s", id, esp_err_to_name(delete_err));
    }
    if ((uint8_t)idx < s_config->profile_count - 1) {
        memmove(&s_config->profiles[idx], &s_config->profiles[idx + 1],
                sizeof(device_manager_profile_t) * (s_config->profile_count - idx - 1));
    }
    s_config->profile_count--;
    if (strcasecmp(s_config->active_profile, id) == 0) {
        s_config->active_profile[0] = 0;
    }
    dm_profiles_ensure_active(s_config);
    dm_profiles_sync_from_active(s_config, true);
    s_config->generation++;
    esp_err_t err = persist_locked();
    dm_unlock();
    return err;
}

esp_err_t device_manager_profile_rename(const char *id, const char *new_name)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!id || !id[0] || !new_name || !new_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    dm_lock();
    dm_profiles_ensure_active(s_config);
    device_manager_profile_t *profile = dm_profiles_find_by_id(s_config, id);
    if (!profile) {
        dm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    dm_str_copy(profile->name, sizeof(profile->name), new_name);
    s_config->generation++;
    esp_err_t err = persist_locked();
    dm_unlock();
    return err;
}

esp_err_t device_manager_profile_activate(const char *id)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    dm_lock();
    dm_profiles_ensure_active(s_config);
    device_manager_profile_t *profile = dm_profiles_find_by_id(s_config, id);
    if (!profile) {
        dm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (strcasecmp(s_config->active_profile, id) == 0) {
        dm_unlock();
        return ESP_OK;
    }
    dm_str_copy(s_config->active_profile, sizeof(s_config->active_profile), profile->id);
    dm_profiles_sync_from_active(s_config, true);
    s_config->generation++;
    esp_err_t err = persist_locked();
    dm_unlock();
    return err;
}

esp_err_t device_manager_apply_json(const char *json, size_t len)
{
    return device_manager_apply_profile_json(NULL, json, len);
}
