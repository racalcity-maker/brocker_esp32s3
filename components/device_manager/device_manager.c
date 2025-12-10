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
#include "device_manager_internal.h"

static const char *TAG = "device_manager";
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
    dm_load_defaults(s_config);
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
        dm_load_defaults(s_config);
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
    bool ok = dm_populate_config_from_json(next, root);
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
