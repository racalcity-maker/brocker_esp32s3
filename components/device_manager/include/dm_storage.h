#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "device_manager.h"

esp_err_t dm_storage_load(const char *path, device_manager_config_t *cfg);
esp_err_t dm_storage_save(const char *path, const device_manager_config_t *cfg);
esp_err_t dm_storage_export_json(const device_manager_config_t *cfg, char **out_json, size_t *out_len);
esp_err_t dm_storage_parse_json(const char *json, size_t len, device_manager_config_t *cfg);

// Internal hooks implemented in core (JSON serializers)
esp_err_t dm_storage_internal_parse(const char *json, size_t len, device_manager_config_t *cfg);
esp_err_t dm_storage_internal_export(const device_manager_config_t *cfg, char **out_json, size_t *out_len);
