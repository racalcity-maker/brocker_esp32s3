#pragma once

#include "esp_err.h"

typedef enum {
    STATUS_LED_PATTERN_OFF = 0,
    STATUS_LED_PATTERN_SOLID_RED,
    STATUS_LED_PATTERN_BLINK_RED,
    STATUS_LED_PATTERN_SOLID_GREEN,
} status_led_pattern_t;

esp_err_t status_led_init(void);
void status_led_set_pattern(status_led_pattern_t pattern);
void status_led_flash_warning(uint8_t flashes);
