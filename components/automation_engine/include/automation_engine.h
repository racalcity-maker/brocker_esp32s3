#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t automation_engine_init(void);
esp_err_t automation_engine_start(void);
void automation_engine_reload(void);
bool automation_engine_handle_mqtt(const char *topic, const char *payload);
esp_err_t automation_engine_trigger(const char *device_id, const char *scenario_id);
void automation_engine_set_variable(const char *key, const char *value);
void automation_engine_clear_variable(const char *key);

#ifdef __cplusplus
}
#endif
