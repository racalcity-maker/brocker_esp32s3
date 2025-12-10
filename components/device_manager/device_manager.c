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
static bool s_config_ready = false;

static device_manager_config_t *dm_config_alloc(uint8_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }
    size_t bytes = sizeof(device_manager_config_t) + sizeof(device_descriptor_t) * capacity;
    device_manager_config_t *cfg = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cfg) {
        cfg = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (cfg) {
        cfg->device_capacity = capacity;
    }
    return cfg;
}

static void dm_config_free(device_manager_config_t *cfg)
{
    if (cfg) {
        heap_caps_free(cfg);
    }
}

static size_t dm_config_device_bytes(const device_manager_config_t *cfg, uint8_t slots)
{
    if (!cfg || slots == 0) {
        return 0;
    }
    return sizeof(device_descriptor_t) * slots;
}

static void dm_config_clone(device_manager_config_t *dest, const device_manager_config_t *src)
{
    if (!dest || !src) {
        return;
    }
    uint8_t dest_cap = dest->device_capacity ? dest->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
    uint8_t src_cap = src->device_capacity ? src->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
    uint8_t src_used = src->device_count;
    if (src_used > src_cap) {
        src_used = src_cap;
    }
    uint8_t copy_slots = (dest_cap < src_used) ? dest_cap : src_used;
    size_t meta = sizeof(device_manager_config_t);
    size_t device_bytes = dm_config_device_bytes(src, copy_slots);
    ESP_LOGI(TAG, "dm_copy size=%zu, psram_free=%u, internal_free=%u",
             meta + device_bytes,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const size_t chunk = 512;
    size_t offset = 0;
    uint8_t *dst_bytes = (uint8_t *)dest;
    const uint8_t *src_bytes = (const uint8_t *)src;
    while (offset < meta) {
        size_t part = chunk;
        if (part > meta - offset) {
            part = meta - offset;
        }
        memcpy(dst_bytes + offset, src_bytes + offset, part);
        offset += part;
        feed_wdt();
    }
    dest->device_capacity = dest_cap;
    uint8_t *dst_devices = (uint8_t *)dest->devices;
    const uint8_t *src_devices = (const uint8_t *)src->devices;
    offset = 0;
    while (offset < device_bytes) {
        size_t part = chunk;
        if (part > device_bytes - offset) {
            part = device_bytes - offset;
        }
        memcpy(dst_devices + offset, src_devices + offset, part);
        offset += part;
        feed_wdt();
    }
    if (dest_cap > copy_slots) {
        size_t remaining_bytes = dm_config_device_bytes(dest, dest_cap - copy_slots);
        memset(dst_devices + device_bytes, 0, remaining_bytes);
    }
    if (dest->device_count > dest_cap) {
        dest->device_count = dest_cap;
    }
}

static void *dm_cjson_malloc(size_t size);
static void dm_cjson_free(void *ptr);
void dm_cjson_install_hooks(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = dm_cjson_malloc,
        .free_fn = dm_cjson_free,
    };
    cJSON_InitHooks(&hooks);
}

void dm_cjson_reset_hooks(void)
{
    cJSON_InitHooks(NULL);
}
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

static void register_templates_from_config(device_manager_config_t *cfg);




static void dm_lock(void)
{
    if (s_lock) {
        while (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            feed_wdt();
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
    ESP_LOGI(TAG, "register_templates_from_config: resetting runtime");
    dm_template_runtime_reset();
    uint8_t limit = cfg->device_capacity ? cfg->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
    for (uint8_t i = 0; i < cfg->device_count && i < limit; ++i) {
        device_descriptor_t *dev = &cfg->devices[i];
        if (dev->template_assigned) {
            ESP_LOGI(TAG, "registering template for %s (type=%d)", dev->id, dev->template_config.type);
            esp_err_t err = dm_template_runtime_register(&dev->template_config, dev->id);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "template runtime register failed for %s: %s", dev->id, esp_err_to_name(err));
            }
        }
    }
    ESP_LOGI(TAG, "register_templates_from_config: done");
}

esp_err_t device_manager_init(void)
{
    ESP_LOGI(TAG, ">>> ENTER device_manager_init()");
    ESP_LOGI(TAG, "PSRAM free: %u, internal free: %u",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "device_manager_init start");
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); }
    if (s_config_ready) {
        ESP_LOGI(TAG, "device_manager already initialized");
        return ESP_OK;
    }
    if (!s_config) {
        s_config = dm_config_alloc(DEVICE_MANAGER_MAX_DEVICES);
        ESP_RETURN_ON_FALSE(s_config != NULL, ESP_ERR_NO_MEM, TAG, "alloc config failed");
        ESP_LOGI(TAG, "allocating config buffer (%zu bytes)",
                 sizeof(device_manager_config_t) +
                     dm_config_device_bytes(s_config, s_config->device_capacity));
    }
    ESP_LOGI(TAG, "loading defaults to config buffer");
    dm_load_defaults(s_config);
    feed_wdt();
    device_manager_config_t *temp = dm_config_alloc(DEVICE_MANAGER_MAX_DEVICES);
    if (!temp) {
        ESP_LOGE(TAG, "no memory for temp config");
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    ESP_LOGI(TAG, "Expected cfg size=%zu",
             sizeof(device_manager_config_t) +
                 dm_config_device_bytes(temp, temp->device_capacity));
    ESP_LOGI(TAG, "loading config from %s", CONFIG_BACKUP_PATH);
    esp_err_t load_err = dm_storage_load(CONFIG_BACKUP_PATH, temp);
    feed_wdt();
    if (load_err == ESP_OK) {
        dm_lock();
        feed_wdt();
        dm_config_clone(s_config, temp);
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
    dm_config_free(temp);
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

const device_manager_config_t *device_manager_lock_config(void)
{
    dm_lock();
    return s_config;
}

void device_manager_unlock_config(void)
{
    dm_unlock();
}

esp_err_t device_manager_reload_from_nvs(void)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    device_manager_config_t *temp = dm_config_alloc(DEVICE_MANAGER_MAX_DEVICES);
    if (!temp) {
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    esp_err_t err = dm_storage_load(CONFIG_BACKUP_PATH, temp);
    feed_wdt();
    if (err != ESP_OK) {
        dm_config_free(temp);
        return err;
    }
    dm_lock();
    feed_wdt();
    dm_config_clone(s_config, temp);
    s_config->generation++;
    dm_profiles_sync_from_active(s_config, true);
    dm_profiles_sync_to_active(s_config);
    feed_wdt();
    dm_unlock();
    dm_config_free(temp);
    register_templates_from_config(s_config);
    return ESP_OK;
}

static esp_err_t persist_locked(void)
{
    uint8_t cap = s_config ? s_config->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
    device_manager_config_t *snapshot = dm_config_alloc(cap);
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    dm_profiles_sync_to_active(s_config);
    esp_err_t profile_err = dm_profiles_store_active(s_config);
    if (profile_err != ESP_OK) {
        dm_config_free(snapshot);
        return profile_err;
    }
    dm_config_clone(snapshot, s_config);
    feed_wdt();
    esp_err_t err = dm_storage_save(CONFIG_BACKUP_PATH, snapshot);
    dm_config_free(snapshot);
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
    dm_config_clone(s_config, next);
    feed_wdt();
    s_config->generation++;
    dm_profiles_sync_to_active(s_config);
    feed_wdt();
    esp_err_t err = persist_locked();
    dm_unlock();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "device_manager_apply: config persisted, re-registering templates");
        register_templates_from_config(s_config);
        event_bus_message_t msg = {
            .type = EVENT_DEVICE_CONFIG_CHANGED,
        };
        event_bus_post(&msg, 0);
    } else {
        ESP_LOGE(TAG, "device_manager_apply: persist failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t device_manager_sync_file(void)
{
    if (!s_config_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    dm_lock();
    dm_profiles_sync_to_active(s_config);
    esp_err_t profile_err = dm_profiles_store_active(s_config);
    if (profile_err != ESP_OK) {
        dm_unlock();
        return profile_err;
    }
    esp_err_t err = dm_storage_save(CONFIG_BACKUP_PATH, s_config);
    dm_unlock();
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
    dm_lock();
    dm_profiles_sync_to_active(s_config);
    bool export_active = (!profile_id || !profile_id[0] ||
                          strcasecmp(profile_id, s_config->active_profile) == 0);
    if (export_active) {
        esp_err_t err = dm_storage_export_json(s_config, out_json, out_len);
        dm_unlock();
        return err;
    }
    uint8_t cap = s_config ? s_config->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
    device_manager_config_t *snapshot = dm_config_alloc(cap);
    if (!snapshot) {
        dm_unlock();
        return ESP_ERR_NO_MEM;
    }
    feed_wdt();
    dm_config_clone(snapshot, s_config);
    dm_unlock();
    dm_str_copy(snapshot->active_profile, sizeof(snapshot->active_profile), profile_id);
    dm_profiles_ensure_active(snapshot);
    dm_profiles_sync_from_active(snapshot, false);
    esp_err_t err = dm_storage_export_json(snapshot, out_json, out_len);
    dm_config_free(snapshot);
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
    uint8_t cap = s_config ? s_config->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
    device_manager_config_t *next = dm_config_alloc(cap);
    if (!next) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    bool ok = dm_populate_config_from_json(next, root);
    cJSON_Delete(root);
    if (!ok) {
        dm_config_free(next);
        return ESP_ERR_INVALID_ARG;
    }
    if (profile_id && profile_id[0]) {
        dm_str_copy(next->active_profile, sizeof(next->active_profile), profile_id);
    }
    feed_wdt();
    esp_err_t err = device_manager_apply(next);
    dm_config_free(next);
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
            esp_err_t clone_err = dm_profiles_load_profile(clone_profile->id,
                                                           s_config->devices,
                                                           s_config->device_capacity,
                                                           &cloned_count);
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
