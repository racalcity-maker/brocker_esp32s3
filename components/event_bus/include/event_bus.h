#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

typedef enum {
    EVENT_NONE = 0,
    EVENT_CARD_OK,
    EVENT_CARD_BAD,
    EVENT_RELAY_CMD,
    EVENT_AUDIO_PLAY,
    EVENT_VOLUME_SET,
    EVENT_WEB_COMMAND,
    EVENT_SYSTEM_STATUS,
    EVENT_DEVICE_CONFIG_CHANGED,
    EVENT_MQTT_MESSAGE,
    EVENT_FLAG_CHANGED,
} event_bus_type_t;

typedef struct {
    event_bus_type_t type;
    char topic[64];
    char payload[256];
} event_bus_message_t;

typedef void (*event_bus_handler_t)(const event_bus_message_t *message);

esp_err_t event_bus_init(void);
esp_err_t event_bus_start(void);
esp_err_t event_bus_post(const event_bus_message_t *message, TickType_t timeout);
esp_err_t event_bus_register_handler(event_bus_handler_t handler);
