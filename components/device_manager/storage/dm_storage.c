#include "dm_storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "device_manager.h"

static const char *TAG = "dm_storage";

esp_err_t dm_storage_load(const char *path, device_manager_config_t *cfg)
{
    if (!path || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "config file %s not found", path);
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
    esp_err_t err = dm_storage_internal_parse(buf, (size_t)size, cfg);
    free(buf);
    return err;
}

esp_err_t dm_storage_save(const char *path, const device_manager_config_t *cfg)
{
    if (!path || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = NULL;
    size_t len = 0;
    esp_err_t err = dm_storage_internal_export(cfg, &json, &len);
    if (err != ESP_OK) {
        return err;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(json);
        ESP_LOGE(TAG, "failed to open %s for write", path);
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

esp_err_t dm_storage_export_json(const device_manager_config_t *cfg, char **out_json, size_t *out_len)
{
    return dm_storage_internal_export(cfg, out_json, out_len);
}

esp_err_t dm_storage_parse_json(const char *json, size_t len, device_manager_config_t *cfg)
{
    return dm_storage_internal_parse(json, len, cfg);
}
