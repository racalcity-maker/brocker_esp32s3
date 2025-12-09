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

static const char *TAG = "device_manager";
static const uint32_t DEVICE_CONFIG_VERSION = 1;
static const char *CONFIG_BACKUP_PATH = "/sdcard/brocker_devices.json";
static const char *DEFAULT_PICTURES_TRACK_OK = "/sdcard/pictures/ok.mp3";
static const char *DEFAULT_PICTURES_TRACK_FAIL = "/sdcard/pictures/fail.mp3";
static const char *DEFAULT_LASER_RELAY_TRACK = "/sdcard/laser/relayOn.mp3";
static const char *DEFAULT_PROFILE_ID = "default";
static const char *DEFAULT_PROFILE_NAME = "Default";

#define DM_DEVICE_MAX DEVICE_MANAGER_MAX_DEVICES
#define DM_SCENARIO_MAX DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE

static SemaphoreHandle_t s_lock;
static device_manager_config_t *s_config = NULL;
static bool s_config_ready = false;

static esp_err_t storage_load(device_manager_config_t *cfg);
static esp_err_t storage_save(const device_manager_config_t *cfg);
static esp_err_t export_json_from_config(const device_manager_config_t *cfg, char **out_json, size_t *out_len);
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

static void str_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = 0;
}

static device_manager_profile_t *find_profile_by_id(device_manager_config_t *cfg, const char *id)
{
    if (!cfg || !id || !id[0]) {
        return NULL;
    }
    for (uint8_t i = 0; i < cfg->profile_count && i < DEVICE_MANAGER_MAX_PROFILES; ++i) {
        device_manager_profile_t *profile = &cfg->profiles[i];
        if (profile->id[0] && strcasecmp(profile->id, id) == 0) {
            return profile;
        }
    }
    return NULL;
}

static void copy_devices_from_profile(device_manager_config_t *cfg, const device_manager_profile_t *profile)
{
    if (!cfg || !profile) {
        return;
    }
    uint8_t count = profile->device_count;
    if (count > DEVICE_MANAGER_MAX_DEVICES) {
        count = DEVICE_MANAGER_MAX_DEVICES;
    }
    cfg->device_count = count;
    if (count > 0) {
        memcpy(cfg->devices, profile->devices, sizeof(device_descriptor_t) * count);
    }
}

static void copy_devices_to_profile(const device_manager_config_t *cfg, device_manager_profile_t *profile)
{
    if (!cfg || !profile) {
        return;
    }
    uint8_t count = cfg->device_count;
    if (count > DEVICE_MANAGER_MAX_DEVICES) {
        count = DEVICE_MANAGER_MAX_DEVICES;
    }
    profile->device_count = count;
    if (count > 0) {
        memcpy(profile->devices, cfg->devices, sizeof(device_descriptor_t) * count);
    }
}

static device_manager_profile_t *ensure_active_profile(device_manager_config_t *cfg)
{
    if (!cfg) {
        return NULL;
    }
    if (cfg->profile_count == 0 && DEVICE_MANAGER_MAX_PROFILES > 0) {
        device_manager_profile_t *profile = &cfg->profiles[0];
        memset(profile, 0, sizeof(*profile));
        str_copy(profile->id, sizeof(profile->id), DEFAULT_PROFILE_ID);
        str_copy(profile->name, sizeof(profile->name), DEFAULT_PROFILE_NAME);
        profile->device_count = cfg->device_count;
        if (profile->device_count > DEVICE_MANAGER_MAX_DEVICES) {
            profile->device_count = DEVICE_MANAGER_MAX_DEVICES;
        }
        if (profile->device_count > 0) {
            memcpy(profile->devices, cfg->devices, sizeof(device_descriptor_t) * profile->device_count);
        }
        cfg->profile_count = 1;
    }
    if (!cfg->active_profile[0] && cfg->profile_count > 0) {
        str_copy(cfg->active_profile, sizeof(cfg->active_profile), cfg->profiles[0].id);
    }
    device_manager_profile_t *profile = find_profile_by_id(cfg, cfg->active_profile);
    if (!profile && cfg->profile_count > 0) {
        profile = &cfg->profiles[0];
        str_copy(cfg->active_profile, sizeof(cfg->active_profile), profile->id);
    }
    return profile;
}

static void sync_devices_from_active_profile(device_manager_config_t *cfg)
{
    device_manager_profile_t *profile = ensure_active_profile(cfg);
    if (!profile) {
        return;
    }
    copy_devices_from_profile(cfg, profile);
}

static void sync_devices_to_active_profile(device_manager_config_t *cfg)
{
    device_manager_profile_t *profile = ensure_active_profile(cfg);
    if (!profile) {
        return;
    }
    copy_devices_to_profile(cfg, profile);
}

static bool profile_id_valid(const char *id)
{
    if (!id || !id[0]) {
        return false;
    }
    size_t len = strlen(id);
    if (len >= DEVICE_MANAGER_ID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!(isalnum(c) || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
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
    {DEVICE_TAB_PICTURES, "pictures"},
    {DEVICE_TAB_LASER, "laser"},
    {DEVICE_TAB_ROBOT, "robot"},
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
    {DEVICE_ACTION_LASER_TRIGGER, "laser_trigger"},
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

static device_descriptor_t *find_device_by_id(device_manager_config_t *cfg, const char *id)
{
    if (!cfg || !id || !id[0]) {
        return NULL;
    }
    for (uint8_t i = 0; i < cfg->device_count && i < DEVICE_MANAGER_MAX_DEVICES; ++i) {
        device_descriptor_t *dev = &cfg->devices[i];
        if ((dev->id[0] && strcasecmp(dev->id, id) == 0) ||
            (dev->display_name[0] && strcasecmp(dev->display_name, id) == 0)) {
            return dev;
        }
    }
    return NULL;
}

static device_scenario_t *find_scenario_by_id(device_descriptor_t *device, const char *id)
{
    if (!device || !id || !id[0]) {
        return NULL;
    }
    for (uint8_t i = 0; i < device->scenario_count && i < DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE; ++i) {
        device_scenario_t *sc = &device->scenarios[i];
        if ((sc->id[0] && strcasecmp(sc->id, id) == 0) ||
            (sc->name[0] && strcasecmp(sc->name, id) == 0)) {
            return sc;
        }
    }
    return NULL;
}

static void load_defaults(device_manager_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->schema_version = DEVICE_CONFIG_VERSION;
    cfg->generation = 1;
    cfg->tab_limit = DEVICE_MANAGER_MAX_TABS;
    cfg->profile_count = 0;
    cfg->active_profile[0] = 0;
    ensure_active_profile(cfg);
    sync_devices_from_active_profile(cfg);
}

static const device_descriptor_t k_default_pictures_device = {
    .id = DEVICE_MANAGER_DEVICE_ID_PICTURES,
    .display_name = "Pictures",
    .tab_count = 0,
    .topic_count = 0,
    .scenario_count = 2,
    .scenarios = {
        {
            .id = DEVICE_MANAGER_SCENARIO_PICTURES_OK,
            .name = "Pictures OK",
            .step_count = 3,
            .steps = {
                {
                    .type = DEVICE_ACTION_MQTT_PUBLISH,
                    .data.mqtt = {
                        .topic = "pictures/lightsgreen",
                        .payload = "ok",
                        .qos = 0,
                        .retain = false,
                    },
                },
                {
                    .type = DEVICE_ACTION_MQTT_PUBLISH,
                    .data.mqtt = {
                        .topic = "robot/speak",
                        .payload = "/sdcard/pictures/ok.mp3",
                        .qos = 0,
                        .retain = false,
                    },
                },
                {
                    .type = DEVICE_ACTION_AUDIO_PLAY,
                    .data.audio = {
                        .track = "/sdcard/pictures/ok.mp3",
                        .blocking = false,
                    },
                },
            },
        },
        {
            .id = DEVICE_MANAGER_SCENARIO_PICTURES_FAIL,
            .name = "Pictures Fail",
            .step_count = 3,
            .steps = {
                {
                    .type = DEVICE_ACTION_MQTT_PUBLISH,
                    .data.mqtt = {
                        .topic = "pictures/lightsred",
                        .payload = "fail",
                        .qos = 0,
                        .retain = false,
                    },
                },
                {
                    .type = DEVICE_ACTION_MQTT_PUBLISH,
                    .data.mqtt = {
                        .topic = "robot/speak",
                        .payload = "/sdcard/pictures/fail.mp3",
                        .qos = 0,
                        .retain = false,
                    },
                },
                {
                    .type = DEVICE_ACTION_AUDIO_PLAY,
                    .data.audio = {
                        .track = "/sdcard/pictures/fail.mp3",
                        .blocking = false,
                    },
                },
            },
        },
    },
};

static const device_descriptor_t k_default_laser_device = {
    .id = DEVICE_MANAGER_DEVICE_ID_LASER,
    .display_name = "Laser",
    .tab_count = 0,
    .topic_count = 0,
    .scenario_count = 1,
    .scenarios = {
        {
            .id = DEVICE_MANAGER_SCENARIO_LASER_TRIGGER,
            .name = "Laser Trigger",
            .step_count = 3,
            .steps = {
                {
                    .type = DEVICE_ACTION_MQTT_PUBLISH,
                    .data.mqtt = {
                        .topic = "relay/relayOn",
                        .payload = "on",
                        .qos = 0,
                        .retain = false,
                    },
                },
                {
                    .type = DEVICE_ACTION_MQTT_PUBLISH,
                    .data.mqtt = {
                        .topic = "robot/laser/relayOn.mp3",
                        .payload = "/sdcard/laser/relayOn.mp3",
                        .qos = 0,
                        .retain = false,
                    },
                },
                {
                    .type = DEVICE_ACTION_AUDIO_PLAY,
                    .data.audio = {
                        .track = "/sdcard/laser/relayOn.mp3",
                        .blocking = false,
                    },
                },
            },
        },
    },
};

static bool append_device_from_template(device_manager_config_t *cfg, const device_descriptor_t *tmpl)
{
    if (!cfg || !tmpl) {
        return false;
    }
    if (cfg->device_count >= DEVICE_MANAGER_MAX_DEVICES) {
        ESP_LOGW(TAG, "device list full, cannot add %s", tmpl->id);
        return false;
    }
    device_descriptor_t *dst = &cfg->devices[cfg->device_count];
    memcpy(dst, tmpl, sizeof(*dst));
    cfg->device_count++;
    return true;
}

static bool ensure_core_devices(device_manager_config_t *cfg)
{
    bool changed = false;
    if (!find_device_by_id(cfg, DEVICE_MANAGER_DEVICE_ID_PICTURES)) {
        if (append_device_from_template(cfg, &k_default_pictures_device)) {
            changed = true;
            ESP_LOGI(TAG, "added default device '%s'", DEVICE_MANAGER_DEVICE_ID_PICTURES);
        }
    }
    if (!find_device_by_id(cfg, DEVICE_MANAGER_DEVICE_ID_LASER)) {
        if (append_device_from_template(cfg, &k_default_laser_device)) {
            changed = true;
            ESP_LOGI(TAG, "added default device '%s'", DEVICE_MANAGER_DEVICE_ID_LASER);
        }
    }
    return changed;
}

static device_action_step_t *find_mqtt_step(device_scenario_t *scenario, const char *topic)
{
    if (!scenario || !topic || !topic[0]) {
        return NULL;
    }
    for (uint8_t i = 0; i < scenario->step_count && i < DEVICE_MANAGER_MAX_STEPS_PER_SCENARIO; ++i) {
        device_action_step_t *step = &scenario->steps[i];
        if (step->type == DEVICE_ACTION_MQTT_PUBLISH &&
            strcasecmp(step->data.mqtt.topic, topic) == 0) {
            return step;
        }
    }
    return NULL;
}

static device_action_step_t *find_audio_step(device_scenario_t *scenario)
{
    if (!scenario) {
        return NULL;
    }
    for (uint8_t i = 0; i < scenario->step_count && i < DEVICE_MANAGER_MAX_STEPS_PER_SCENARIO; ++i) {
        device_action_step_t *step = &scenario->steps[i];
        if (step->type == DEVICE_ACTION_AUDIO_PLAY) {
            return step;
        }
    }
    return NULL;
}

static esp_err_t parse_config_json(const char *json, size_t len, device_manager_config_t *cfg)
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

static esp_err_t storage_load(device_manager_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(CONFIG_BACKUP_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "config file %s not found", CONFIG_BACKUP_PATH);
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);
    char *buf = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read_bytes != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }
    buf[size] = '\0';
    esp_err_t err = parse_config_json(buf, (size_t)size, cfg);
    free(buf);
    return err;
}

static esp_err_t export_json_from_config(const device_manager_config_t *cfg, char **out_json, size_t *out_len);

static esp_err_t storage_save(const device_manager_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = NULL;
    size_t len = 0;
    esp_err_t err = export_json_from_config(cfg, &json, &len);
    if (err != ESP_OK) {
        return err;
    }
    FILE *f = fopen(CONFIG_BACKUP_PATH, "wb");
    if (!f) {
        free(json);
        ESP_LOGE(TAG, "failed to open %s for write", CONFIG_BACKUP_PATH);
        return ESP_FAIL;
    }
    size_t written = fwrite(json, 1, len, f);
    fclose(f);
    free(json);
    if (written != len) {
        ESP_LOGE(TAG, "failed to write config file (%zu/%zu)", written, len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void ensure_storage(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
}

esp_err_t device_manager_init(void)
{
    ESP_LOGI(TAG, ">>> ENTER device_manager_init()");
    ESP_LOGI(TAG, "PSRAM free: %u, internal free: %u",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "device_manager_init start");
    ensure_storage();
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

    device_manager_config_t *temp = heap_caps_calloc(1, sizeof(*temp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!temp) {
        ESP_LOGE(TAG, "no memory for temp config");
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    ESP_LOGI(TAG, "Expected cfg size=%zu", sizeof(*temp));
    ESP_LOGI(TAG, "loading config from %s", CONFIG_BACKUP_PATH);
    esp_err_t load_err = storage_load(temp);
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
        ESP_ERROR_CHECK_WITHOUT_ABORT(storage_save(s_config));
        dm_unlock();
    }
    free(temp);
    sync_devices_from_active_profile(s_config);
    bool defaults_added = ensure_core_devices(s_config);
    if (defaults_added) {
        s_config->generation++;
        ESP_ERROR_CHECK_WITHOUT_ABORT(storage_save(s_config));
    }
    sync_devices_to_active_profile(s_config);
    s_config_ready = true;
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
    esp_err_t err = storage_load(temp);
    feed_wdt();
    if (err != ESP_OK) {
        free(temp);
        return err;
    }
    dm_lock();
    feed_wdt();
    dm_copy(s_config, temp, sizeof(*temp));
    s_config->generation++;
    sync_devices_from_active_profile(s_config);
    bool defaults_added = ensure_core_devices(s_config);
    if (defaults_added) {
        s_config->generation++;
        ESP_ERROR_CHECK_WITHOUT_ABORT(storage_save(s_config));
    }
    sync_devices_to_active_profile(s_config);
    feed_wdt();
    dm_unlock();
    free(temp);
    return ESP_OK;
}

static esp_err_t persist_locked(void)
{
    device_manager_config_t *snapshot = heap_caps_calloc(1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    sync_devices_to_active_profile(s_config);
    dm_copy(snapshot, s_config, sizeof(*snapshot));
    feed_wdt();
    esp_err_t err = storage_save(snapshot);
    free(snapshot);
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
    bool defaults_added = ensure_core_devices(s_config);
    if (defaults_added) {
        s_config->generation++;
    }
    sync_devices_to_active_profile(s_config);
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
    device_manager_config_t *snapshot = heap_caps_calloc(1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    dm_lock();
    sync_devices_to_active_profile(s_config);
    dm_copy(snapshot, s_config, sizeof(*snapshot));
    dm_unlock();
    esp_err_t err = storage_save(snapshot);
    free(snapshot);
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
    case DEVICE_ACTION_LASER_TRIGGER:
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
        str_copy(step->data.mqtt.topic, sizeof(step->data.mqtt.topic),
                 cJSON_GetStringValue(cJSON_GetObjectItem(obj, "topic")));
        str_copy(step->data.mqtt.payload, sizeof(step->data.mqtt.payload),
                 cJSON_GetStringValue(cJSON_GetObjectItem(obj, "payload")));
        step->data.mqtt.qos = (uint8_t)json_number_to_u32(cJSON_GetObjectItem(obj, "qos"), 0);
        step->data.mqtt.retain = json_get_bool_default(cJSON_GetObjectItem(obj, "retain"), false);
        break;
    case DEVICE_ACTION_AUDIO_PLAY:
        str_copy(step->data.audio.track, sizeof(step->data.audio.track),
                 cJSON_GetStringValue(cJSON_GetObjectItem(obj, "track")));
        step->data.audio.blocking = json_get_bool_default(cJSON_GetObjectItem(obj, "blocking"), false);
        break;
    case DEVICE_ACTION_SET_FLAG:
        str_copy(step->data.flag.flag, sizeof(step->data.flag.flag),
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
                str_copy(dst->flag, sizeof(dst->flag), flag->valuestring);
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
        str_copy(step->data.event.event, sizeof(step->data.event.event),
                 cJSON_GetStringValue(cJSON_GetObjectItem(obj, "event")));
        str_copy(step->data.event.topic, sizeof(step->data.event.topic),
                 cJSON_GetStringValue(cJSON_GetObjectItem(obj, "topic")));
        str_copy(step->data.event.payload, sizeof(step->data.event.payload),
                 cJSON_GetStringValue(cJSON_GetObjectItem(obj, "payload")));
        break;
    case DEVICE_ACTION_AUDIO_STOP:
    case DEVICE_ACTION_LASER_TRIGGER:
    case DEVICE_ACTION_DELAY:
    case DEVICE_ACTION_NOP:
    default:
        break;
    }
    return true;
}

static esp_err_t export_json_from_config(const device_manager_config_t *cfg, char **out_json, size_t *out_len)
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
    const char *active_profile = cfg->active_profile[0] ? cfg->active_profile : DEFAULT_PROFILE_ID;
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
    device_manager_config_t *snapshot = heap_caps_calloc(1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    dm_lock();
    feed_wdt();
    dm_copy(snapshot, s_config, sizeof(*snapshot));
    feed_wdt();
    dm_unlock();
    if (profile_id && profile_id[0]) {
        str_copy(snapshot->active_profile, sizeof(snapshot->active_profile), profile_id);
    }
    ensure_active_profile(snapshot);
    sync_devices_from_active_profile(snapshot);
    esp_err_t err = export_json_from_config(snapshot, out_json, out_len);
    heap_caps_free(snapshot);
    return err;
}

esp_err_t device_manager_update_pictures_tracks(const char *ok_track, const char *fail_track)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *ok = (ok_track && ok_track[0]) ? ok_track : DEFAULT_PICTURES_TRACK_OK;
    const char *fail = (fail_track && fail_track[0]) ? fail_track : DEFAULT_PICTURES_TRACK_FAIL;
    dm_lock();
    device_descriptor_t *dev = find_device_by_id(s_config, DEVICE_MANAGER_DEVICE_ID_PICTURES);
    if (!dev) {
        dm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    bool changed = false;
    device_scenario_t *ok_scen = find_scenario_by_id(dev, DEVICE_MANAGER_SCENARIO_PICTURES_OK);
    if (ok_scen) {
        device_action_step_t *robot = find_mqtt_step(ok_scen, "robot/speak");
        device_action_step_t *audio = find_audio_step(ok_scen);
        if (robot) {
            str_copy(robot->data.mqtt.payload, sizeof(robot->data.mqtt.payload), ok);
            changed = true;
        }
        if (audio) {
            str_copy(audio->data.audio.track, sizeof(audio->data.audio.track), ok);
            changed = true;
        }
    }
    device_scenario_t *fail_scen = find_scenario_by_id(dev, DEVICE_MANAGER_SCENARIO_PICTURES_FAIL);
    if (fail_scen) {
        device_action_step_t *robot = find_mqtt_step(fail_scen, "robot/speak");
        device_action_step_t *audio = find_audio_step(fail_scen);
        if (robot) {
            str_copy(robot->data.mqtt.payload, sizeof(robot->data.mqtt.payload), fail);
            changed = true;
        }
        if (audio) {
            str_copy(audio->data.audio.track, sizeof(audio->data.audio.track), fail);
            changed = true;
        }
    }
    esp_err_t err = ESP_OK;
    if (changed) {
        s_config->generation++;
        sync_devices_to_active_profile(s_config);
        err = storage_save(s_config);
    }
    dm_unlock();
    return err;
}

esp_err_t device_manager_update_laser_relay_track(const char *relay_track)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *track = (relay_track && relay_track[0]) ? relay_track : DEFAULT_LASER_RELAY_TRACK;
    dm_lock();
    device_descriptor_t *dev = find_device_by_id(s_config, DEVICE_MANAGER_DEVICE_ID_LASER);
    if (!dev) {
        dm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    device_scenario_t *sc = find_scenario_by_id(dev, DEVICE_MANAGER_SCENARIO_LASER_TRIGGER);
    bool changed = false;
    if (sc) {
        device_action_step_t *robot = find_mqtt_step(sc, "robot/laser/relayOn.mp3");
        device_action_step_t *audio = find_audio_step(sc);
        if (robot) {
            str_copy(robot->data.mqtt.payload, sizeof(robot->data.mqtt.payload), track);
            changed = true;
        }
        if (audio) {
            str_copy(audio->data.audio.track, sizeof(audio->data.audio.track), track);
            changed = true;
        }
    }
    esp_err_t err = ESP_OK;
    if (changed) {
        s_config->generation++;
        err = storage_save(s_config);
    }
    dm_unlock();
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
            device_manager_profile_t *profile = find_profile_by_id(cfg, id_item->valuestring);
            if (!profile && cfg->profile_count < DEVICE_MANAGER_MAX_PROFILES) {
                profile = &cfg->profiles[cfg->profile_count++];
                memset(profile, 0, sizeof(*profile));
                str_copy(profile->id, sizeof(profile->id), id_item->valuestring);
            }
            if (!profile) {
                continue;
            }
            const cJSON *name_item = cJSON_GetObjectItem(node, "name");
            if (cJSON_IsString(name_item) && name_item->valuestring) {
                str_copy(profile->name, sizeof(profile->name), name_item->valuestring);
            }
        }
    }
    const cJSON *active = cJSON_GetObjectItem(root, "active_profile");
    if (cJSON_IsString(active) && active->valuestring && active->valuestring[0]) {
        str_copy(cfg->active_profile, sizeof(cfg->active_profile), active->valuestring);
    }
    ensure_active_profile(cfg);

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
        str_copy(dev->id, sizeof(dev->id), cJSON_GetStringValue(cJSON_GetObjectItem(dev_node, "id")));
        str_copy(dev->display_name, sizeof(dev->display_name),
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
                str_copy(tab->label, sizeof(tab->label),
                         cJSON_GetStringValue(cJSON_GetObjectItem(tab_node, "label")));
                str_copy(tab->extra_payload, sizeof(tab->extra_payload),
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
                str_copy(binding->name, sizeof(binding->name),
                         cJSON_GetStringValue(cJSON_GetObjectItem(topic_node, "name")));
                str_copy(binding->topic, sizeof(binding->topic),
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
                str_copy(sc->id, sizeof(sc->id), cJSON_GetStringValue(cJSON_GetObjectItem(sc_node, "id")));
                str_copy(sc->name, sizeof(sc->name), cJSON_GetStringValue(cJSON_GetObjectItem(sc_node, "name")));
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
        dev_count++;
        feed_wdt();
    }
    cfg->device_count = dev_count;
    sync_devices_to_active_profile(cfg);
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
    device_manager_config_t *next = heap_caps_calloc(1, sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    bool ok = populate_config_from_json(next, root);
    cJSON_Delete(root);
    if (!ok) {
        free(next);
        return ESP_ERR_INVALID_ARG;
    }
    if (profile_id && profile_id[0]) {
        str_copy(next->active_profile, sizeof(next->active_profile), profile_id);
    }
    feed_wdt();
    esp_err_t err = device_manager_apply(next);
    free(next);
    return err;
}

esp_err_t device_manager_profile_create(const char *id, const char *name, const char *clone_id)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!profile_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    dm_lock();
    ensure_active_profile(s_config);
    if (find_profile_by_id(s_config, id)) {
        dm_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config->profile_count >= DEVICE_MANAGER_MAX_PROFILES) {
        dm_unlock();
        return ESP_ERR_NO_MEM;
    }
    device_manager_profile_t *dst = &s_config->profiles[s_config->profile_count++];
    memset(dst, 0, sizeof(*dst));
    str_copy(dst->id, sizeof(dst->id), id);
    str_copy(dst->name, sizeof(dst->name), (name && name[0]) ? name : id);
    const device_manager_profile_t *src = NULL;
    if (clone_id && clone_id[0]) {
        src = find_profile_by_id(s_config, clone_id);
    }
    if (!src) {
        src = ensure_active_profile(s_config);
    }
    if (src) {
        memcpy(dst->devices, src->devices, sizeof(dst->devices));
        dst->device_count = src->device_count;
    }
    str_copy(s_config->active_profile, sizeof(s_config->active_profile), dst->id);
    sync_devices_from_active_profile(s_config);
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
    ensure_active_profile(s_config);
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
    if ((uint8_t)idx < s_config->profile_count - 1) {
        memmove(&s_config->profiles[idx], &s_config->profiles[idx + 1],
                sizeof(device_manager_profile_t) * (s_config->profile_count - idx - 1));
    }
    s_config->profile_count--;
    if (strcasecmp(s_config->active_profile, id) == 0) {
        s_config->active_profile[0] = 0;
    }
    ensure_active_profile(s_config);
    sync_devices_from_active_profile(s_config);
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
    ensure_active_profile(s_config);
    device_manager_profile_t *profile = find_profile_by_id(s_config, id);
    if (!profile) {
        dm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    str_copy(profile->name, sizeof(profile->name), new_name);
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
    ensure_active_profile(s_config);
    device_manager_profile_t *profile = find_profile_by_id(s_config, id);
    if (!profile) {
        dm_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (strcasecmp(s_config->active_profile, id) == 0) {
        dm_unlock();
        return ESP_OK;
    }
    str_copy(s_config->active_profile, sizeof(s_config->active_profile), profile->id);
    sync_devices_from_active_profile(s_config);
    s_config->generation++;
    esp_err_t err = persist_locked();
    dm_unlock();
    return err;
}

esp_err_t device_manager_apply_json(const char *json, size_t len)
{
    return device_manager_apply_profile_json(NULL, json, len);
}
