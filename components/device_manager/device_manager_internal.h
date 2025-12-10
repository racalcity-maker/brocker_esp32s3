#pragma once

#include "device_manager.h"
#include "cJSON.h"
#include "esp_task_wdt.h"

#define DM_DEVICE_CONFIG_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

bool dm_populate_config_from_json(device_manager_config_t *cfg, const cJSON *root);
void dm_load_defaults(device_manager_config_t *cfg);

const char *dm_condition_to_string(device_condition_type_t cond);
bool dm_condition_from_string(const char *name, device_condition_type_t *out);
const char *dm_tab_type_to_string(device_tab_type_t type);
bool dm_tab_type_from_string(const char *name, device_tab_type_t *out);
const char *dm_action_type_to_string(device_action_type_t type);
bool dm_action_type_from_string(const char *name, device_action_type_t *out);

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

#ifdef __cplusplus
}
#endif
