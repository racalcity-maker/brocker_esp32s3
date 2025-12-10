#pragma once

#include "esp_err.h"
#include "device_manager.h"
#include "dm_template_registry.h"

esp_err_t dm_template_apply_to_config(device_manager_config_t *cfg,
                                      const dm_template_config_t *tpl,
                                      const char *device_id,
                                      const char *display_name);
