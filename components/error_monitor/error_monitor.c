#include "error_monitor.h"

#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "error_monitor";
static SemaphoreHandle_t s_lock = NULL;
static bool s_wifi_ok = false;
static bool s_sd_present = false;
static bool s_sd_fault = false;

static void update_led_locked(void)
{
    status_led_pattern_t pattern = STATUS_LED_PATTERN_OFF;
    if (!s_sd_present || s_sd_fault) {
        pattern = STATUS_LED_PATTERN_BLINK_RED;
    } else if (!s_wifi_ok) {
        pattern = STATUS_LED_PATTERN_SOLID_RED;
    } else {
        pattern = STATUS_LED_PATTERN_SOLID_GREEN;
    }
    status_led_set_pattern(pattern);
}

esp_err_t error_monitor_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_RETURN_ON_ERROR(status_led_init(), TAG, "led init");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_ok = false;
    s_sd_present = false;
    s_sd_fault = false;
    update_led_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "error monitor ready");
    return ESP_OK;
}

void error_monitor_set_wifi_connected(bool connected)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_ok = connected;
    update_led_locked();
    xSemaphoreGive(s_lock);
}

void error_monitor_set_sd_state(bool mounted)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sd_present = mounted;
    if (mounted) {
        s_sd_fault = false;
    }
    update_led_locked();
    xSemaphoreGive(s_lock);
}

void error_monitor_report_sd_fault(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sd_fault = true;
    update_led_locked();
    xSemaphoreGive(s_lock);
}

void error_monitor_report_audio_fault(void)
{
    status_led_flash_warning(3);
}
