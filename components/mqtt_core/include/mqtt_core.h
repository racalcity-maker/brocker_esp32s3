#pragma once

#include "esp_err.h"
#ifndef MQTT_CORE_DEBUG
#define MQTT_CORE_DEBUG 0
#endif
#include "event_bus.h"

esp_err_t mqtt_core_init(void);
esp_err_t mqtt_core_start(void);

// Публикация наружу (клиенты MQTT получат сообщение).
esp_err_t mqtt_core_publish(const char *topic, const char *payload);

// Инъекция входящего MQTT сообщения в шину событий (парсинг топика -> event type).
esp_err_t mqtt_core_inject_message(const char *topic, const char *payload);

// Вернуть топик по типу события (если известен).
const char *mqtt_core_topic_for_event(event_bus_type_t type);
uint8_t mqtt_core_client_count(void);
typedef struct {
    uint8_t total;
} mqtt_client_stats_t;
void mqtt_core_get_client_stats(mqtt_client_stats_t *out);
