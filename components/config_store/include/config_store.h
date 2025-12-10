#pragma once

#include <stdint.h>

#include "esp_err.h"

#define CONFIG_STORE_MAX_MQTT_USERS   16
#define CONFIG_STORE_CLIENT_ID_MAX    32
#define CONFIG_STORE_USERNAME_MAX     32
#define CONFIG_STORE_PASSWORD_MAX     32
#define CONFIG_STORE_AUTH_HASH_LEN    32

typedef struct {
    char ssid[32];
    char password[64];
    char hostname[32];
} app_wifi_config_t;

typedef struct {
    char client_id[CONFIG_STORE_CLIENT_ID_MAX];
    char username[CONFIG_STORE_USERNAME_MAX];
    char password[CONFIG_STORE_PASSWORD_MAX];
} app_mqtt_user_t;

typedef struct {
    char broker_id[16];
    int port;
    int keepalive_seconds;
    uint8_t user_count;
    app_mqtt_user_t users[CONFIG_STORE_MAX_MQTT_USERS];
} app_mqtt_config_t;

typedef struct {
    char username[CONFIG_STORE_USERNAME_MAX];
    uint8_t password_hash[CONFIG_STORE_AUTH_HASH_LEN];
} app_web_auth_t;

typedef struct {
    char ntp_server[64];
    int timezone_offset_min;
} app_time_config_t;

typedef struct {
    app_wifi_config_t wifi;
    app_mqtt_config_t mqtt;
    app_time_config_t time;
    app_web_auth_t web;
} app_config_t;

esp_err_t config_store_init(void);
const app_config_t *config_store_get(void);
esp_err_t config_store_set(const app_config_t *next);
esp_err_t config_store_reset_defaults(void);
void config_store_hash_password(const char *password, uint8_t out_hash[CONFIG_STORE_AUTH_HASH_LEN]);
esp_err_t config_store_set_web_auth(const char *username, const uint8_t hash[CONFIG_STORE_AUTH_HASH_LEN]);
esp_err_t config_store_reset_web_auth_defaults(void);
