#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_err.h"

#include "config_store.h"
#include "network.h"
#include "mqtt_core.h"
#include "web_ui.h"
#include "audio_player.h"
#include "event_bus.h"
#include "error_monitor.h"
#include "device_manager.h"
#include "automation_engine.h"
#include "esp_task_wdt.h"
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "app_main";

#if (configUSE_TRACE_FACILITY == 1)
static void stats_task(void *param)
{
    (void)param;
    const TickType_t period = pdMS_TO_TICKS(10000);
    vTaskDelay(period);
    while (1) {
        UBaseType_t count = uxTaskGetNumberOfTasks();
        TaskStatus_t *tasks = calloc(count, sizeof(TaskStatus_t));
        uint32_t total = 0;
        uint32_t idle = 0;
        if (tasks) {
            count = uxTaskGetSystemState(tasks, count, &total);
            for (UBaseType_t i = 0; i < count; ++i) {
                const char *name = tasks[i].pcTaskName;
                if (name && strstr(name, "IDLE")) {
                    idle += tasks[i].ulRunTimeCounter;
                }
            }
        }

        float cpu = 0.0f;
        if (total > 0 && idle <= total) {
            cpu = ((float)(total - idle) * 100.0f) / (float)total;
        }
        uint32_t heap_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t heap_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint32_t min_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (heap_int < 50 * 1024 || min_int < 50 * 1024) {
            ESP_LOGW("stats", "low heap: heap_int=%" PRIu32 " min_int=%" PRIu32 " psram=%" PRIu32 " cpu=%.1f%% tasks=%" PRIu32,
                     heap_int, min_int, heap_spiram, cpu, (uint32_t)count);
        }
        if (tasks) {
            free(tasks);
        }
        vTaskDelay(period);
    }
}
#endif  // configUSE_TRACE_FACILITY

static void device_manager_boot_task(void *param)
{
    bool wdt_registered = false;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err == ESP_OK) {
        wdt_registered = true;
    } else {
        ESP_LOGW(TAG, "failed to add dm_boot to task WDT: %s", esp_err_to_name(wdt_err));
    }
    ESP_LOGI(TAG, "device manager bootstrap start");
    esp_err_t err = device_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_manager_init failed: %s", esp_err_to_name(err));
        goto exit;
    }
    ESP_LOGI(TAG, "device manager ready, init automation engine");
    err = automation_engine_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "automation_engine_init failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = automation_engine_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "automation_engine_start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "automation engine started");
    }
exit:
    if (wdt_registered) {
        esp_task_wdt_delete(NULL);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    static const esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = false,
    };
    esp_task_wdt_init(&twdt_cfg);

    ESP_LOGI(TAG, "init nvs_flash");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "init config_store");
    ESP_ERROR_CHECK(config_store_init());
    ESP_LOGI(TAG, "init event_bus");
    ESP_ERROR_CHECK(event_bus_init());
    ESP_LOGI(TAG, "init error_monitor");
    ESP_ERROR_CHECK(error_monitor_init());
    ESP_LOGI(TAG, "init network");
    ESP_ERROR_CHECK(network_init());
    ESP_LOGI(TAG, "init mqtt_core");
    ESP_ERROR_CHECK(mqtt_core_init());
    ESP_LOGI(TAG, "init audio_player");
    ESP_ERROR_CHECK(audio_player_init());
    ESP_LOGI(TAG, "init web_ui");
    ESP_ERROR_CHECK(web_ui_init());

    ESP_LOGI(TAG, "start event_bus");
    event_bus_start();
    ESP_LOGI(TAG, "start network");
    network_start();
    ESP_LOGI(TAG, "start mqtt_core");
    mqtt_core_start();
    ESP_LOGI(TAG, "start audio_player");
    audio_player_start();
    ESP_LOGI(TAG, "start web_ui");
    web_ui_start();

#if (configUSE_TRACE_FACILITY == 1)
    xTaskCreate(stats_task, "stats_task", 4096, NULL, 3, NULL);
#else
    ESP_LOGW(TAG, "stats task disabled (configUSE_TRACE_FACILITY not enabled)");
#endif
    ESP_LOGI(TAG, "Broker skeleton started");
    ESP_LOGI(TAG, "create device manager bootstrap task");
    xTaskCreate(device_manager_boot_task, "dm_boot", 8192, NULL, 3, NULL);
    vTaskDelete(NULL);
}
