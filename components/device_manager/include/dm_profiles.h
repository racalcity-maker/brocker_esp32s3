#pragma once

#include <stdbool.h>
#include "device_manager.h"

#define DM_DEFAULT_PROFILE_ID   "default"
#define DM_DEFAULT_PROFILE_NAME "Default"

device_manager_profile_t *dm_profiles_find_by_id(device_manager_config_t *cfg, const char *id);
device_manager_profile_t *dm_profiles_ensure_active(device_manager_config_t *cfg);
void dm_profiles_sync_from_active(device_manager_config_t *cfg, bool create_if_missing);
void dm_profiles_sync_to_active(device_manager_config_t *cfg);
bool dm_profiles_id_valid(const char *id);
esp_err_t dm_profiles_store_active(const device_manager_config_t *cfg);
esp_err_t dm_profiles_load_profile(const char *profile_id, device_descriptor_t *devices, uint8_t *device_count);
esp_err_t dm_profiles_delete_profile_file(const char *profile_id);
