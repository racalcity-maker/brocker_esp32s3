#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t error_monitor_init(void);
void error_monitor_set_wifi_connected(bool connected);
void error_monitor_set_sd_state(bool mounted);
void error_monitor_report_sd_fault(void);
void error_monitor_report_audio_fault(void);
