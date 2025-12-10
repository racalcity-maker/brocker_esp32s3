#include "config_store.h"

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"
#include "sdkconfig.h"

#ifndef CONFIG_BROKER_WEB_AUTH_DEFAULT_USER
#define CONFIG_BROKER_WEB_AUTH_DEFAULT_USER "admin"
#endif

#ifndef CONFIG_BROKER_WEB_AUTH_DEFAULT_PASS
#define CONFIG_BROKER_WEB_AUTH_DEFAULT_PASS "admin"
#endif

static const char *TAG = "config_store";
static const char *NVS_NS = "cfg";
static const uint32_t CONFIG_VERSION = 1;
static app_config_t g_config;
static portMUX_TYPE g_config_lock = portMUX_INITIALIZER_UNLOCKED;

static void config_lock(void)
{
    taskENTER_CRITICAL(&g_config_lock);
}

static void config_unlock(void)
{
    taskEXIT_CRITICAL(&g_config_lock);
}

void config_store_hash_password(const char *password, uint8_t out_hash[CONFIG_STORE_AUTH_HASH_LEN])
{
    if (!out_hash) {
        return;
    }
    const unsigned char *input = (const unsigned char *)(password ? password : "");
    size_t len = strlen((const char *)input);
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000
    if (mbedtls_sha256_starts(&ctx, 0) != 0 ||
        mbedtls_sha256_update(&ctx, input, len) != 0 ||
        mbedtls_sha256_finish(&ctx, out_hash) != 0) {
#else
    if (mbedtls_sha256_starts_ret(&ctx, 0) != 0 ||
        mbedtls_sha256_update_ret(&ctx, input, len) != 0 ||
        mbedtls_sha256_finish_ret(&ctx, out_hash) != 0) {
#endif
        memset(out_hash, 0, CONFIG_STORE_AUTH_HASH_LEN);
    }
    mbedtls_sha256_free(&ctx);
}

static void apply_default_web_auth(app_web_auth_t *web)
{
    if (!web) {
        return;
    }
    memset(web, 0, sizeof(*web));
    strncpy(web->username, CONFIG_BROKER_WEB_AUTH_DEFAULT_USER, sizeof(web->username) - 1);
    config_store_hash_password(CONFIG_BROKER_WEB_AUTH_DEFAULT_PASS, web->password_hash);
}

static void load_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    // Пустой SSID => стартуем AP для конфигурации.
    cfg->wifi.ssid[0] = '\0';
    cfg->wifi.password[0] = '\0';
    strncpy(cfg->wifi.hostname, "brocker", sizeof(cfg->wifi.hostname) - 1);
    strncpy(cfg->mqtt.broker_id, "brocker", sizeof(cfg->mqtt.broker_id) - 1);
    cfg->mqtt.port = 1883;
    cfg->mqtt.keepalive_seconds = 30;
    cfg->mqtt.user_count = 0;
    strncpy(cfg->time.ntp_server, "pool.ntp.org", sizeof(cfg->time.ntp_server) - 1);
    cfg->time.timezone_offset_min = 180;
    apply_default_web_auth(&cfg->web);
}

static bool validate_string(const char *s, size_t max_len)
{
    if (!s) {
        return false;
    }
    size_t len = strnlen(s, max_len);
    return len > 0 && len < max_len;
}

static bool hash_is_nonzero(const uint8_t *hash)
{
    if (!hash) {
        return false;
    }
    for (size_t i = 0; i < CONFIG_STORE_AUTH_HASH_LEN; ++i) {
        if (hash[i] != 0) {
            return true;
        }
    }
    return false;
}

static bool validate_web_auth(const app_web_auth_t *web)
{
    if (!web) {
        return false;
    }
    if (!validate_string(web->username, sizeof(web->username))) {
        return false;
    }
    return hash_is_nonzero(web->password_hash);
}

static bool validate_mqtt_user(const app_mqtt_user_t *user)
{
    if (!user) {
        return false;
    }
    if (!validate_string(user->client_id, sizeof(user->client_id))) {
        return false;
    }
    if (!validate_string(user->username, sizeof(user->username))) {
        return false;
    }
    if (!validate_string(user->password, sizeof(user->password))) {
        return false;
    }
    return true;
}

static bool validate_config(const app_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    // Wi-Fi SSID может быть пустым (тогда запускаем AP для конфигурации).
    if (cfg->wifi.hostname[0] != '\0' && !validate_string(cfg->wifi.hostname, sizeof(cfg->wifi.hostname))) {
        return false;
    }
    if (!validate_string(cfg->mqtt.broker_id, sizeof(cfg->mqtt.broker_id))) {
        return false;
    }
    if (cfg->mqtt.port <= 0 || cfg->mqtt.port > 65535) {
        return false;
    }
    if (cfg->mqtt.keepalive_seconds <= 0 || cfg->mqtt.keepalive_seconds > 600) {
        return false;
    }
    if (cfg->mqtt.user_count > CONFIG_STORE_MAX_MQTT_USERS) {
        return false;
    }
    for (uint8_t i = 0; i < cfg->mqtt.user_count; ++i) {
        if (!validate_mqtt_user(&cfg->mqtt.users[i])) {
            return false;
        }
    }
    if (!validate_string(cfg->time.ntp_server, sizeof(cfg->time.ntp_server))) {
        return false;
    }
    if (!validate_web_auth(&cfg->web)) {
        return false;
    }
    return true;
}

static esp_err_t save_to_nvs(const app_config_t *cfg)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &handle), TAG, "nvs_open failed");

    esp_err_t err = nvs_set_u32(handle, "ver", CONFIG_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "cfg", cfg, sizeof(*cfg));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_from_nvs(app_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t ver = 0;
    size_t size = sizeof(*cfg);
    err = nvs_get_u32(handle, "ver", &ver);
    if (err != ESP_OK || ver != CONFIG_VERSION) {
        nvs_close(handle);
        return ESP_ERR_INVALID_VERSION;
    }
    memset(cfg, 0, sizeof(*cfg));
    err = nvs_get_blob(handle, "cfg", cfg, &size);
    nvs_close(handle);
    if (err == ESP_OK && size > sizeof(*cfg)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t config_store_init(void)
{
    app_config_t defaults;
    load_defaults(&defaults);
    config_lock();
    g_config = defaults;
    config_unlock();

    app_config_t *loaded = malloc(sizeof(app_config_t));
    if (loaded && load_from_nvs(loaded) == ESP_OK && validate_config(loaded)) {
        config_lock();
        g_config = *loaded;
        config_unlock();
        free(loaded);
        ESP_LOGI(TAG, "config loaded from NVS");
        return ESP_OK;
    }
    if (loaded) {
        free(loaded);
    }

    ESP_LOGW(TAG, "using default config (failed to load or invalid)");
    app_config_t *snapshot = malloc(sizeof(app_config_t));
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    config_lock();
    *snapshot = g_config;
    config_unlock();
    esp_err_t err = save_to_nvs(snapshot);
    free(snapshot);
    return err;
}

const app_config_t *config_store_get(void)
{
    return &g_config;
}

esp_err_t config_store_set(const app_config_t *next)
{
    if (!validate_config(next)) {
        ESP_LOGE(TAG, "config validation failed");
        return ESP_ERR_INVALID_ARG;
    }
    app_config_t *snapshot = malloc(sizeof(app_config_t));
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    config_lock();
    g_config = *next;
    *snapshot = g_config;
    config_unlock();
    esp_err_t err = save_to_nvs(snapshot);
    free(snapshot);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "config saved to NVS");
    } else {
        ESP_LOGE(TAG, "failed to save config: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_store_reset_defaults(void)
{
    app_config_t *snapshot = malloc(sizeof(app_config_t));
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    app_config_t defaults;
    load_defaults(&defaults);
    config_lock();
    g_config = defaults;
    *snapshot = g_config;
    config_unlock();
    esp_err_t err = save_to_nvs(snapshot);
    free(snapshot);
    return err;
}

esp_err_t config_store_set_web_auth(const char *username, const uint8_t hash[CONFIG_STORE_AUTH_HASH_LEN])
{
    if (!username || !hash) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!validate_string(username, CONFIG_STORE_USERNAME_MAX) || !hash_is_nonzero(hash)) {
        return ESP_ERR_INVALID_ARG;
    }
    app_config_t *snapshot = malloc(sizeof(app_config_t));
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    config_lock();
    *snapshot = g_config;
    config_unlock();
    memset(snapshot->web.username, 0, sizeof(snapshot->web.username));
    strncpy(snapshot->web.username, username, sizeof(snapshot->web.username) - 1);
    memcpy(snapshot->web.password_hash, hash, CONFIG_STORE_AUTH_HASH_LEN);
    esp_err_t err = config_store_set(snapshot);
    free(snapshot);
    return err;
}

esp_err_t config_store_reset_web_auth_defaults(void)
{
    app_config_t *snapshot = malloc(sizeof(app_config_t));
    if (!snapshot) {
        return ESP_ERR_NO_MEM;
    }
    config_lock();
    *snapshot = g_config;
    config_unlock();
    apply_default_web_auth(&snapshot->web);
    esp_err_t err = config_store_set(snapshot);
    free(snapshot);
    return err;
}
