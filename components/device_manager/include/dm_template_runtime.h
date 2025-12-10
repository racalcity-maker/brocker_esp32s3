#pragma once

#include <stdbool.h>
#include "esp_err.h"

#include "dm_template_registry.h"

esp_err_t dm_template_runtime_init(void);
void dm_template_runtime_reset(void);
esp_err_t dm_template_runtime_register(const dm_template_config_t *tpl, const char *device_id);
bool dm_template_runtime_handle_mqtt(const char *topic, const char *payload);
bool dm_template_runtime_handle_flag(const char *flag_name, bool state);
typedef struct {
    char device_id[DEVICE_MANAGER_ID_MAX_LEN];
    uint8_t slot_count;
    struct {
        char source_id[DEVICE_MANAGER_ID_MAX_LEN];
        char label[DEVICE_MANAGER_NAME_MAX_LEN];
        bool has_value;
        char last_value[DM_UID_TEMPLATE_VALUE_MAX_LEN];
    } slots[DM_UID_TEMPLATE_MAX_SLOTS];
} dm_uid_runtime_snapshot_t;
esp_err_t dm_template_runtime_get_uid_snapshot(const char *device_id, dm_uid_runtime_snapshot_t *out);
