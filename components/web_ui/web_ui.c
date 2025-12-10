#include "web_ui.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "event_bus.h"
#include "config_store.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "network.h"
#include "audio_player.h"
#include <sys/stat.h>
#include "mqtt_core.h"
#include "device_manager.h"
#include "automation_engine.h"
#include "dm_template_runtime.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "web_ui_page.h"
#include "web_ui_utils.h"
#include "web_ui_devices.h"

#ifndef CONFIG_BROKER_WEB_AUTH_RESET_GPIO
#define CONFIG_BROKER_WEB_AUTH_RESET_GPIO -1
#endif

#define WEB_SESSION_MAX             6
#define WEB_SESSION_TOKEN_LEN       64
#define WEB_SESSION_TTL_US          (12LL * 60 * 60 * 1000000)
#define WEB_AUTH_RESET_HOLD_US      (10LL * 1000000)

typedef esp_err_t (*web_handler_fn)(httpd_req_t *);

typedef struct {
    web_handler_fn fn;
    bool redirect_on_fail;
} web_route_t;

typedef struct {
    bool in_use;
    char token[WEB_SESSION_TOKEN_LEN];
    int64_t expires_at;
} web_session_entry_t;

static const char *TAG = "web_ui";
static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_scan_mutex = NULL;
static SemaphoreHandle_t s_session_mutex = NULL;
static web_session_entry_t s_sessions[WEB_SESSION_MAX];
#if CONFIG_BROKER_WEB_AUTH_RESET_GPIO >= 0
static TaskHandle_t s_reset_task = NULL;
#endif

static esp_err_t devices_templates_handler(httpd_req_t *req);
static char *build_uid_monitor_json(void);
static char *build_mqtt_users_json(const app_mqtt_config_t *mqtt_cfg);
static esp_err_t mqtt_users_handler(httpd_req_t *req);
static bool web_ui_require_session(httpd_req_t *req, bool redirect_on_fail);
static esp_err_t auth_gate_handler(httpd_req_t *req);
static esp_err_t login_page_handler(httpd_req_t *req);
static esp_err_t auth_login_handler(httpd_req_t *req);
static esp_err_t auth_logout_handler(httpd_req_t *req);
static esp_err_t auth_password_handler(httpd_req_t *req);
static void web_sessions_init(void);
static void web_sessions_clear(void);
static esp_err_t register_guarded_route(const char *uri, httpd_method_t method, const web_route_t *route);
static esp_err_t register_public_route(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *));
static char *dup_empty_json_array(void)
{
    char *buf = malloc(3);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, "[]", 3);
    return buf;
}

static char *build_uid_monitor_json(void)
{
    const device_manager_config_t *cfg = device_manager_lock_config();
    if (!cfg) {
        device_manager_unlock_config();
        return dup_empty_json_array();
    }
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        device_manager_unlock_config();
        return dup_empty_json_array();
    }
    uint8_t limit = cfg->device_capacity ? cfg->device_capacity : DEVICE_MANAGER_MAX_DEVICES;
    for (uint8_t i = 0; i < cfg->device_count && i < limit; ++i) {
        const device_descriptor_t *dev = &cfg->devices[i];
        if (!dev->template_assigned || dev->template_config.type != DM_TEMPLATE_TYPE_UID) {
            continue;
        }
        cJSON *dev_obj = cJSON_CreateObject();
        if (!dev_obj) {
            cJSON_Delete(root);
            device_manager_unlock_config();
            return dup_empty_json_array();
        }
        cJSON_AddStringToObject(dev_obj, "id", dev->id);
        cJSON_AddStringToObject(dev_obj, "name", dev->display_name[0] ? dev->display_name : dev->id);
        cJSON *slot_arr = cJSON_AddArrayToObject(dev_obj, "slots");
        if (!slot_arr) {
            cJSON_Delete(root);
            device_manager_unlock_config();
            return dup_empty_json_array();
        }
        dm_uid_runtime_snapshot_t snapshot;
        bool have_snapshot = (dm_template_runtime_get_uid_snapshot(dev->id, &snapshot) == ESP_OK);
        const dm_uid_template_t *tpl = &dev->template_config.data.uid;
        uint8_t slot_count = tpl->slot_count;
        if (slot_count > DM_UID_TEMPLATE_MAX_SLOTS) {
            slot_count = DM_UID_TEMPLATE_MAX_SLOTS;
        }
        for (uint8_t s = 0; s < slot_count; ++s) {
            const dm_uid_slot_t *slot_cfg = &tpl->slots[s];
            if (!slot_cfg->source_id[0]) {
                continue;
            }
            cJSON *slot_obj = cJSON_CreateObject();
            if (!slot_obj) {
                cJSON_Delete(root);
                device_manager_unlock_config();
                return dup_empty_json_array();
            }
            cJSON_AddNumberToObject(slot_obj, "index", s);
            cJSON_AddStringToObject(slot_obj, "source", slot_cfg->source_id);
            if (slot_cfg->label[0]) {
                cJSON_AddStringToObject(slot_obj, "label", slot_cfg->label);
            }
            if (have_snapshot && s < snapshot.slot_count && snapshot.slots[s].has_value &&
                snapshot.slots[s].last_value[0]) {
                cJSON_AddStringToObject(slot_obj, "last_value", snapshot.slots[s].last_value);
            }
            cJSON_AddItemToArray(slot_arr, slot_obj);
        }
        cJSON_AddItemToArray(root, dev_obj);
    }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    device_manager_unlock_config();
    if (!printed) {
        return dup_empty_json_array();
    }
    return printed;
}

static char *build_mqtt_users_json(const app_mqtt_config_t *mqtt_cfg)
{
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return dup_empty_json_array();
    }
    if (mqtt_cfg) {
        for (uint8_t i = 0; i < mqtt_cfg->user_count && i < CONFIG_STORE_MAX_MQTT_USERS; ++i) {
            const app_mqtt_user_t *user = &mqtt_cfg->users[i];
            if (!user->client_id[0]) {
                continue;
            }
            cJSON *obj = cJSON_CreateObject();
            if (!obj) {
                cJSON_Delete(root);
                return dup_empty_json_array();
            }
            cJSON_AddStringToObject(obj, "client_id", user->client_id);
            cJSON_AddStringToObject(obj, "username", user->username);
            cJSON_AddStringToObject(obj, "password", user->password);
            cJSON_AddItemToArray(root, obj);
        }
    }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return dup_empty_json_array();
    }
    return printed;
}

static void web_sessions_init(void)
{
    if (!s_session_mutex) {
        s_session_mutex = xSemaphoreCreateMutex();
    }
    if (s_session_mutex) {
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        memset(s_sessions, 0, sizeof(s_sessions));
        xSemaphoreGive(s_session_mutex);
    }
}

static void web_sessions_clear(void)
{
    if (!s_session_mutex) {
        return;
    }
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    memset(s_sessions, 0, sizeof(s_sessions));
    xSemaphoreGive(s_session_mutex);
}

static void web_session_generate_token(char *out, size_t len)
{
    static const char *hex = "0123456789abcdef";
    if (!out || len < 33) {
        return;
    }
    for (size_t i = 0; i < (len - 1) / 2; ++i) {
        uint32_t r = esp_random();
        out[i * 2] = hex[(r >> 4) & 0x0F];
        out[i * 2 + 1] = hex[r & 0x0F];
    }
    out[len - 1] = 0;
}

static void web_session_store(const char *token)
{
    if (!token || !token[0] || !s_session_mutex) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    web_session_entry_t *slot = NULL;
    for (size_t i = 0; i < WEB_SESSION_MAX; ++i) {
        web_session_entry_t *entry = &s_sessions[i];
        if (entry->in_use && strcmp(entry->token, token) == 0) {
            slot = entry;
            break;
        }
        if (!entry->in_use || entry->expires_at < now) {
            slot = entry;
        }
    }
    if (slot) {
        memset(slot->token, 0, sizeof(slot->token));
        strncpy(slot->token, token, sizeof(slot->token) - 1);
        slot->expires_at = now + WEB_SESSION_TTL_US;
        slot->in_use = true;
    }
    xSemaphoreGive(s_session_mutex);
}

static bool web_session_validate(const char *token)
{
    if (!token || !token[0] || !s_session_mutex) {
        return false;
    }
    bool valid = false;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    for (size_t i = 0; i < WEB_SESSION_MAX; ++i) {
        web_session_entry_t *entry = &s_sessions[i];
        if (!entry->in_use) {
            continue;
        }
        if (entry->expires_at < now) {
            entry->in_use = false;
            continue;
        }
        if (strcmp(entry->token, token) == 0) {
            entry->expires_at = now + WEB_SESSION_TTL_US;
            valid = true;
            break;
        }
    }
    xSemaphoreGive(s_session_mutex);
    return valid;
}

static void web_session_remove(const char *token)
{
    if (!token || !token[0] || !s_session_mutex) {
        return;
    }
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    for (size_t i = 0; i < WEB_SESSION_MAX; ++i) {
        web_session_entry_t *entry = &s_sessions[i];
        if (entry->in_use && strcmp(entry->token, token) == 0) {
            entry->in_use = false;
            break;
        }
    }
    xSemaphoreGive(s_session_mutex);
}

static bool read_cookie_value(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    if (!req || !name || !out || out_len == 0) {
        return false;
    }
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hdr_len <= 0 || hdr_len >= 512) {
        return false;
    }
    char *buf = malloc(hdr_len + 1);
    if (!buf) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, hdr_len + 1) != ESP_OK) {
        free(buf);
        return false;
    }
    bool found = false;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    char *start = strstr(buf, pattern);
    if (start) {
        start += strlen(pattern);
        char *end = strchr(start, ';');
        size_t copy_len = end ? (size_t)(end - start) : strlen(start);
        if (copy_len >= out_len) {
            copy_len = out_len - 1;
        }
        memcpy(out, start, copy_len);
        out[copy_len] = 0;
        found = true;
    }
    free(buf);
    return found;
}

static bool web_ui_require_session(httpd_req_t *req, bool redirect_on_fail)
{
    if (!req) {
        return false;
    }
    char token[WEB_SESSION_TOKEN_LEN] = {0};
    if (read_cookie_value(req, "broker_sid", token, sizeof(token)) && web_session_validate(token)) {
        return true;
    }
    if (redirect_on_fail) {
        char location[128];
        const char *prefix = "/login?next=";
        const char *next_uri = req->uri[0] ? req->uri : "/";
        size_t prefix_len = strlen(prefix);
        size_t copy_len = sizeof(location) > prefix_len ? sizeof(location) - prefix_len - 1 : 0;
        memcpy(location, prefix, prefix_len);
        location[prefix_len] = '\0';
        if (copy_len > 0) {
            strncat(location, next_uri, copy_len);
        }
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", location);
        httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
    }
    return false;
}

static char *read_request_body(httpd_req_t *req, size_t max_len)
{
    if (!req) {
        return NULL;
    }
    size_t len = req->content_len;
    if (len == 0 || len > max_len) {
        return NULL;
    }
    char *body = malloc(len + 1);
    if (!body) {
        return NULL;
    }
    size_t received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, body + received, len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            return NULL;
        }
        received += (size_t)r;
    }
    body[len] = 0;
    return body;
}

static esp_err_t auth_gate_handler(httpd_req_t *req)
{
    const web_route_t *route = (const web_route_t *)req->user_ctx;
    if (!route || !route->fn) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "route missing");
    }
    if (!web_ui_require_session(req, route->redirect_on_fail)) {
        return ESP_OK;
    }
    return route->fn(req);
}

static esp_err_t login_page_handler(httpd_req_t *req)
{
    char token[WEB_SESSION_TOKEN_LEN] = {0};
    if (read_cookie_value(req, "broker_sid", token, sizeof(token)) && web_session_validate(token)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, web_ui_get_login_html(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    char *body = read_request_body(req, 1024);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body required");
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *username_item = cJSON_GetObjectItem(json, "username");
    const cJSON *password_item = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(username_item) || !cJSON_IsString(password_item)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
    }
    const char *username = username_item->valuestring;
    const char *password = password_item->valuestring;
    uint8_t hash[CONFIG_STORE_AUTH_HASH_LEN] = {0};
    config_store_hash_password(password, hash);
    const app_config_t *cfg = config_store_get();
    if (!cfg || strcasecmp(cfg->web.username, username) != 0 ||
        memcmp(cfg->web.password_hash, hash, CONFIG_STORE_AUTH_HASH_LEN) != 0) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_Delete(json);
    char token[WEB_SESSION_TOKEN_LEN] = {0};
    web_session_generate_token(token, sizeof(token));
    web_session_store(token);
    char cookie[WEB_SESSION_TOKEN_LEN + 48];
    snprintf(cookie, sizeof(cookie), "broker_sid=%s; Path=/; HttpOnly", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t auth_logout_handler(httpd_req_t *req)
{
    char token[WEB_SESSION_TOKEN_LEN] = {0};
    if (read_cookie_value(req, "broker_sid", token, sizeof(token))) {
        web_session_remove(token);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "broker_sid=deleted; Path=/; Max-Age=0; HttpOnly");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t auth_password_handler(httpd_req_t *req)
{
    char *body = read_request_body(req, 1024);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body required");
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *new_user_item = cJSON_GetObjectItem(json, "username");
    const cJSON *current_item = cJSON_GetObjectItem(json, "current_password");
    const cJSON *next_item = cJSON_GetObjectItem(json, "new_password");
    const char *new_user = cJSON_IsString(new_user_item) ? new_user_item->valuestring : NULL;
    const char *current = cJSON_IsString(current_item) ? current_item->valuestring : NULL;
    const char *next = cJSON_IsString(next_item) ? next_item->valuestring : NULL;
    if (!current || !next || !next[0]) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing password");
    }
    const app_config_t *cfg = config_store_get();
    if (!cfg) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config missing");
    }
    uint8_t hash[CONFIG_STORE_AUTH_HASH_LEN] = {0};
    config_store_hash_password(current, hash);
    if (memcmp(cfg->web.password_hash, hash, CONFIG_STORE_AUTH_HASH_LEN) != 0) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"invalid\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    const char *username = (new_user && new_user[0]) ? new_user : cfg->web.username;
    uint8_t new_hash[CONFIG_STORE_AUTH_HASH_LEN];
    config_store_hash_password(next, new_hash);
    esp_err_t err = config_store_set_web_auth(username, new_hash);
    if (err != ESP_OK) {
        cJSON_Delete(json);
        ESP_LOGE(TAG, "failed to update web auth: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    cJSON_Delete(json);
    web_sessions_clear();
    httpd_resp_set_hdr(req, "Set-Cookie", "broker_sid=deleted; Path=/; Max-Age=0; HttpOnly");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static void web_auth_start_reset_monitor(void);

#if CONFIG_BROKER_WEB_AUTH_RESET_GPIO >= 0
static void web_auth_reset_task(void *param)
{
    const int pin = CONFIG_BROKER_WEB_AUTH_RESET_GPIO;
    int64_t low_since = 0;
    const TickType_t delay_ticks = pdMS_TO_TICKS(100);
    ESP_LOGI(TAG, "web auth reset monitor on GPIO%d", pin);
    while (1) {
        int level = gpio_get_level(pin);
        int64_t now = esp_timer_get_time();
        if (level == 0) {
            if (low_since == 0) {
                low_since = now;
            } else if (now - low_since >= WEB_AUTH_RESET_HOLD_US) {
                ESP_LOGW(TAG, "web auth reset pin triggered, restoring defaults");
                config_store_reset_web_auth_defaults();
                web_sessions_clear();
                low_since = 0;
            }
        } else {
            low_since = 0;
        }
        vTaskDelay(delay_ticks);
    }
}

static void web_auth_start_reset_monitor(void)
{
    if (s_reset_task) {
        return;
    }
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_BROKER_WEB_AUTH_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    xTaskCreate(web_auth_reset_task, "web_auth_reset", 2048, NULL, 5, &s_reset_task);
}
#else
static void web_auth_start_reset_monitor(void)
{
    // feature disabled
}
#endif

static esp_err_t register_guarded_route(const char *uri, httpd_method_t method, const web_route_t *route)
{
    if (!s_server || !route || !route->fn) {
        return ESP_ERR_INVALID_STATE;
    }
    httpd_uri_t desc = {
        .uri = uri,
        .method = method,
        .handler = auth_gate_handler,
        .user_ctx = (void *)route,
    };
    return httpd_register_uri_handler(s_server, &desc);
}

static esp_err_t register_public_route(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    if (!s_server || !handler) {
        return ESP_ERR_INVALID_STATE;
    }
    httpd_uri_t desc = {
        .uri = uri,
        .method = method,
        .handler = handler,
    };
    return httpd_register_uri_handler(s_server, &desc);
}


static esp_err_t ping_handler(httpd_req_t *req)
{
    return web_ui_send_ok(req, "text/plain", "pong");
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if (!s_scan_mutex) {
        s_scan_mutex = xSemaphoreCreateMutex();
    }
    if (!xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(5000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan busy");
    }
    wifi_scan_config_t scan_conf = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_conf, true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_scan_mutex);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t *aps = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!aps) {
        xSemaphoreGive(s_scan_mutex);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    esp_wifi_scan_get_ap_records(&ap_num, aps);

    char *buf = malloc(512);
    if (!buf) {
        free(aps);
        xSemaphoreGive(s_scan_mutex);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    size_t len = 0;
    buf[len++] = '[';
    for (int i = 0; i < ap_num; ++i) {
        char entry[80];
        int w = snprintf(entry, sizeof(entry), "\"%s\"%s", aps[i].ssid, (i == ap_num - 1) ? "" : ",");
        if (w < 0 || len + w + 2 >= 512) break;
        memcpy(buf + len, entry, w);
        len += w;
    }
    buf[len++] = ']';
    buf[len] = 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);

    free(buf);
    free(aps);
    xSemaphoreGive(s_scan_mutex);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    const app_config_t *cfg = config_store_get();
    char ip_buf[32] = "";
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip.ip));
    }
    char *uid_json = build_uid_monitor_json();
    char *mqtt_users_json = build_mqtt_users_json(&cfg->mqtt);
    const char *fmt =
        "{\"wifi\":{\"ssid\":\"%s\",\"host\":\"%s\",\"sta_ip\":\"%s\",\"ap\":%s},"
        "\"mqtt\":{\"id\":\"%s\",\"port\":%d,\"keepalive\":%d,\"users\":%s},"
        "\"audio\":{\"volume\":%d,\"playing\":%s,\"paused\":%s,\"progress\":%d,\"pos_ms\":%d,\"dur_ms\":%d,"
        "\"bitrate\":%d,\"path\":\"%s\",\"message\":\"%s\",\"fmt\":%d},"
        "\"web\":{\"username\":\"%s\"},"
        "\"sd\":{\"ok\":%s,\"total\":%llu,\"free\":%llu},"
        "\"clients\":{\"total\":%u},"
        "\"uid_monitor\":%s}";
    mqtt_client_stats_t stats;
    mqtt_core_get_client_stats(&stats);
    audio_player_status_t a_status;
    audio_player_get_status(&a_status);
    uint64_t kb_total = 0, kb_free = 0;
    bool sd_ok = (esp_vfs_fat_info("/sdcard", &kb_total, &kb_free) == ESP_OK);
    uint64_t sd_total = sd_ok ? kb_total : 0;
    uint64_t sd_free = sd_ok ? kb_free : 0;
    int needed = snprintf(NULL, 0, fmt,
                          cfg->wifi.ssid, cfg->wifi.hostname, ip_buf, network_is_ap_mode() ? "true" : "false",
                          cfg->mqtt.broker_id, cfg->mqtt.port, cfg->mqtt.keepalive_seconds,
                          mqtt_users_json ? mqtt_users_json : "[]",
                          audio_player_get_volume(),
                          a_status.playing ? "true" : "false",
                          a_status.paused ? "true" : "false",
                          a_status.progress,
                          a_status.pos_ms,
                          a_status.dur_ms,
                          a_status.bitrate_kbps,
                          a_status.path,
                          a_status.message,
                          a_status.fmt,
                          cfg->web.username,
                          sd_ok ? "true" : "false",
                          (unsigned long long)sd_total,
                          (unsigned long long)sd_free,
                          stats.total,
                          uid_json ? uid_json : "[]");
    if (needed < 0) {
        if (uid_json) {
            free(uid_json);
        }
        if (mqtt_users_json) {
            free(mqtt_users_json);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status format err");
    }
    size_t buf_len = (size_t)needed + 1;
    char *buf = heap_caps_malloc(buf_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        if (uid_json) {
            free(uid_json);
        }
        if (mqtt_users_json) {
            free(mqtt_users_json);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    snprintf(buf, buf_len, fmt,
             cfg->wifi.ssid, cfg->wifi.hostname, ip_buf, network_is_ap_mode() ? "true" : "false",
             cfg->mqtt.broker_id, cfg->mqtt.port, cfg->mqtt.keepalive_seconds,
             mqtt_users_json ? mqtt_users_json : "[]",
             audio_player_get_volume(),
             a_status.playing ? "true" : "false",
             a_status.paused ? "true" : "false",
             a_status.progress,
             a_status.pos_ms,
             a_status.dur_ms,
             a_status.bitrate_kbps,
             a_status.path,
             a_status.message,
             a_status.fmt,
             cfg->web.username,
             sd_ok ? "true" : "false",
             (unsigned long long)sd_total,
             (unsigned long long)sd_free,
             stats.total,
             uid_json ? uid_json : "[]");
    esp_err_t res = web_ui_send_ok(req, "application/json", buf);
    heap_caps_free(buf);
    if (uid_json) {
        free(uid_json);
    }
    if (mqtt_users_json) {
        free(mqtt_users_json);
    }
    return res;
}

static esp_err_t devices_config_handler(httpd_req_t *req)
{
    char *json = NULL;
    size_t len = 0;
    char query[128];
    char profile[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "profile", profile, sizeof(profile));
    }
    esp_err_t err = profile[0] ? device_manager_export_profile_json(profile, &json, &len)
                               : device_manager_export_json(&json, &len);
    if (err != ESP_OK || !json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "device config unavailable");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json, len);
    heap_caps_free(json);
    return res;
}

static esp_err_t devices_apply_handler(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > 128 * 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char *body = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    size_t received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, body + received, len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            heap_caps_free(body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        received += (size_t)r;
    }
    body[len] = 0;
    char query[128];
    char profile[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "profile", profile, sizeof(profile));
    }
    esp_err_t err = device_manager_apply_profile_json(profile[0] ? profile : NULL, body, len);
    heap_caps_free(body);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }
    device_manager_sync_file();
    return web_ui_send_ok(req, "application/json", "{\"status\":\"ok\"}");
}

static esp_err_t devices_run_handler(httpd_req_t *req)
{
    char query[192] = {0};
    char devid[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    char scenid[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "device", devid, sizeof(devid));
        httpd_query_key_value(query, "scenario", scenid, sizeof(scenid));
    }
    if (!devid[0] || !scenid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "device/scenario required");
    }
    esp_err_t err = automation_engine_trigger(devid, scenid);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }
    return web_ui_send_ok(req, "application/json", "{\"status\":\"queued\"}");
}

static esp_err_t devices_profile_create_handler(httpd_req_t *req)
{
    char query[256];
    char id[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    char name[DEVICE_MANAGER_NAME_MAX_LEN] = {0};
    char clone[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id, sizeof(id));
        httpd_query_key_value(query, "name", name, sizeof(name));
        httpd_query_key_value(query, "clone", clone, sizeof(clone));
    }
    if (!id[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id required");
    }
    esp_err_t err = device_manager_profile_create(id, name[0] ? name : NULL, clone[0] ? clone : NULL);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }
    return web_ui_send_ok(req, "application/json", "{\"status\":\"ok\"}");
}

static esp_err_t devices_profile_delete_handler(httpd_req_t *req)
{
    char query[128];
    char id[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id, sizeof(id));
    }
    if (!id[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id required");
    }
    esp_err_t err = device_manager_profile_delete(id);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }
    return web_ui_send_ok(req, "application/json", "{\"status\":\"ok\"}");
}

static esp_err_t devices_profile_rename_handler(httpd_req_t *req)
{
    char query[256];
    char id[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    char name[DEVICE_MANAGER_NAME_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id, sizeof(id));
        httpd_query_key_value(query, "name", name, sizeof(name));
    }
    if (!id[0] || !name[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id/name required");
    }
    esp_err_t err = device_manager_profile_rename(id, name);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }
    return web_ui_send_ok(req, "application/json", "{\"status\":\"ok\"}");
}

static esp_err_t devices_profile_activate_handler(httpd_req_t *req)
{
    char query[128];
    char id[DEVICE_MANAGER_ID_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id, sizeof(id));
    }
    if (!id[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id required");
    }
    esp_err_t err = device_manager_profile_activate(id);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }
    return web_ui_send_ok(req, "application/json", "{\"status\":\"ok\"}");
}

static esp_err_t devices_variables_handler(httpd_req_t *req)
{
    return web_ui_send_ok(req, "application/json", "[]");
}

static esp_err_t wifi_config_handler(httpd_req_t *req)
{
    char q[160];
    char ssid[32] = {0}, pass[64] = {0}, host[32] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "ssid", ssid, sizeof(ssid));
        httpd_query_key_value(q, "password", pass, sizeof(pass));
        httpd_query_key_value(q, "host", host, sizeof(host));
    }
    app_config_t cfg = *config_store_get();
    if (ssid[0]) strncpy(cfg.wifi.ssid, ssid, sizeof(cfg.wifi.ssid) - 1);
    if (pass[0]) strncpy(cfg.wifi.password, pass, sizeof(cfg.wifi.password) - 1);
    if (host[0]) strncpy(cfg.wifi.hostname, host, sizeof(cfg.wifi.hostname) - 1);
    esp_err_t err = config_store_set(&cfg);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save wifi");
    }
    err = network_apply_wifi_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network_apply_wifi_config failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to apply wifi");
    }
    return web_ui_send_ok(req, "text/plain", "wifi saved");
}

static esp_err_t mqtt_config_handler(httpd_req_t *req)
{
    char q[160];
    char id[16] = {0}, port[8] = {0}, keep[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "id", id, sizeof(id));
        httpd_query_key_value(q, "port", port, sizeof(port));
        httpd_query_key_value(q, "keepalive", keep, sizeof(keep));
    }
    app_config_t cfg = *config_store_get();
    if (id[0]) strncpy(cfg.mqtt.broker_id, id, sizeof(cfg.mqtt.broker_id) - 1);
    if (port[0]) cfg.mqtt.port = atoi(port);
    if (keep[0]) cfg.mqtt.keepalive_seconds = atoi(keep);
    ESP_ERROR_CHECK(config_store_set(&cfg));
    return web_ui_send_ok(req, "text/plain", "mqtt saved");
}

static esp_err_t mqtt_users_handler(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char *body = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    size_t received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, body + received, len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            heap_caps_free(body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        received += (size_t)r;
    }
    body[len] = 0;
    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsArray(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        heap_caps_free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "array required");
    }
    app_config_t cfg = *config_store_get();
    cfg.mqtt.user_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (cfg.mqtt.user_count >= CONFIG_STORE_MAX_MQTT_USERS) {
            cJSON_Delete(root);
            heap_caps_free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too many users");
        }
        if (!cJSON_IsObject(item)) {
            cJSON_Delete(root);
            heap_caps_free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid user entry");
        }
        const cJSON *client = cJSON_GetObjectItem(item, "client_id");
        const cJSON *username = cJSON_GetObjectItem(item, "username");
        const cJSON *password = cJSON_GetObjectItem(item, "password");
        if (!cJSON_IsString(client) || !cJSON_IsString(username) || !cJSON_IsString(password)) {
            cJSON_Delete(root);
            heap_caps_free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
        }
        app_mqtt_user_t *dst = &cfg.mqtt.users[cfg.mqtt.user_count++];
        strncpy(dst->client_id, client->valuestring, sizeof(dst->client_id) - 1);
        dst->client_id[sizeof(dst->client_id) - 1] = 0;
        strncpy(dst->username, username->valuestring, sizeof(dst->username) - 1);
        dst->username[sizeof(dst->username) - 1] = 0;
        strncpy(dst->password, password->valuestring, sizeof(dst->password) - 1);
        dst->password[sizeof(dst->password) - 1] = 0;
    }
    cJSON_Delete(root);
    heap_caps_free(body);
    esp_err_t err = config_store_set(&cfg);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    return web_ui_send_ok(req, "text/plain", "mqtt users saved");
}

static bool path_allowed(const char *path)
{
    if (!path) return false;
    const char *root = "/sdcard";
    size_t root_len = strlen(root);
    return strncmp(path, root, root_len) == 0;
}

static esp_err_t files_handler(httpd_req_t *req)
{
    char q[320];
    char path_enc[256] = {0};
    char dir_path[256] = "/sdcard";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "path", path_enc, sizeof(path_enc));
    }
    if (path_enc[0]) {
        web_ui_url_decode(dir_path, sizeof(dir_path), path_enc);
        if (!path_allowed(dir_path)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        }
    }

    esp_err_t sd_err = audio_player_mount_sd();
    if (sd_err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "sd not mounted: %s", esp_err_to_name(sd_err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }
    DIR *d = opendir(dir_path);
    if (!d) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd not mounted");
    }
    httpd_resp_set_hdr(req, "Connection", "close");
    size_t resp_cap = 4096;
    char *resp = heap_caps_calloc(1, resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        closedir(d);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    size_t len = 0;
    resp[len++] = '[';
    struct dirent *ent;
    bool first = true;
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        // skip . and ..
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) {
            continue;
        }
        if (strcasecmp(n, "System Volume Information") == 0 || strcasecmp(n, "SYSTEM~1") == 0) {
            continue;
        }
        char full[200];
        int written = snprintf(full, sizeof(full), "%s/%s", dir_path, n);
        if (written <= 0 || written >= (int)sizeof(full)) {
            continue;
        }
        struct stat st;
        bool stat_ok = (stat(full, &st) == 0);
        bool is_dir = stat_ok ? S_ISDIR(st.st_mode) : false;
        bool ext_audio = false;
        {
            size_t nlen = strlen(n);
            if (nlen >= 4) {
                const char *ext = n + nlen - 4;
                ext_audio = (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".ogg") == 0);
            }
        }
        if (!stat_ok && !ext_audio) {
            // Could not stat, but not an audio file by ext — treat as dir fallback so it is visible.
            is_dir = true;
        }
        if (!is_dir) {
            size_t nlen = strlen(n);
            if (nlen < 4) continue;
            const char *ext = n + nlen - 4;
            if (strcasecmp(ext, ".wav") != 0 && strcasecmp(ext, ".mp3") != 0 && strcasecmp(ext, ".ogg") != 0) {
                continue;
            }
        }
        long size_bytes = (is_dir || !stat_ok) ? 0 : st.st_size;
        int dur = 0;
        if (!is_dir) {
            size_t nlen = strlen(n);
            const char *ext = n + nlen - 4;
            if (strcasecmp(ext, ".wav") == 0) {
                FILE *wf = fopen(full, "rb");
                if (wf) {
                    struct __attribute__((packed)) wav_header {
                        char riff[4];
                        uint32_t size;
                        char wave[4];
                        char fmt[4];
                        uint32_t fmt_size;
                        uint16_t audio_format;
                        uint16_t num_channels;
                        uint32_t sample_rate;
                        uint32_t byte_rate;
                        uint16_t block_align;
                        uint16_t bits_per_sample;
                        char data_id[4];
                        uint32_t data_size;
                    } hdr;
                    if (fread(&hdr, 1, sizeof(hdr), wf) == sizeof(hdr) &&
                        strncmp(hdr.riff, "RIFF", 4) == 0 && strncmp(hdr.wave, "WAVE", 4) == 0 &&
                        hdr.audio_format == 1 && hdr.byte_rate > 0) {
                        dur = hdr.data_size / (int)hdr.byte_rate;
                    }
                    fclose(wf);
                }
            }
        }
        char entry[300];
        int w = snprintf(entry, sizeof(entry), "%s{\"path\":\"%s\",\"size\":%ld,\"dur\":%d,\"dir\":%s}",
                         first ? "" : ",", full, size_bytes, dur, is_dir ? "true" : "false");
        if (w < 0) {
            continue;
        }
        if (len + (size_t)w + 2 >= resp_cap) {
            size_t new_cap = resp_cap * 2;
            while (new_cap <= len + (size_t)w + 2) new_cap *= 2;
            char *nresp = heap_caps_realloc(resp, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nresp) {
                free(resp);
                closedir(d);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
            }
            resp = nresp;
            resp_cap = new_cap;
        }
        memcpy(resp + len, entry, (size_t)w);
        len += (size_t)w;
        first = false;
    }
    closedir(d);
    if (len + 2 >= resp_cap) {
        char *nresp = heap_caps_realloc(resp, resp_cap + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nresp) {
            resp = nresp;
            resp_cap += 2;
        }
    }
    resp[len++] = ']';
    resp[len] = 0;
    esp_err_t r = web_ui_send_ok(req, "application/json", resp);
    free(resp);
    return r;
}

static esp_err_t audio_play_handler(httpd_req_t *req)
{
    char query[520];
    char path_enc[512] = {0};
    char path[512] = {0};
    char vol[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc));
        httpd_query_key_value(query, "volume", vol, sizeof(vol));
    }
    web_ui_url_decode(path, sizeof(path), path_enc);
    esp_err_t sd_err = audio_player_mount_sd();
    if (sd_err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "sd not mounted: %s", esp_err_to_name(sd_err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }
    if (path[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path required");
    }
    if (vol[0]) {
        audio_player_set_volume(atoi(vol));
    }
    event_bus_message_t msg = {.type = EVENT_AUDIO_PLAY};
    strncpy(msg.payload, path, sizeof(msg.payload) - 1);
    event_bus_post(&msg, pdMS_TO_TICKS(50));
    return web_ui_send_ok(req, "text/plain", "play");
}

static esp_err_t audio_stop_handler(httpd_req_t *req)
{
    audio_player_stop();
    return web_ui_send_ok(req, "text/plain", "stop");
}

static esp_err_t audio_pause_handler(httpd_req_t *req)
{
    audio_player_pause();
    return web_ui_send_ok(req, "text/plain", "pause");
}

static esp_err_t audio_resume_handler(httpd_req_t *req)
{
    audio_player_resume();
    return web_ui_send_ok(req, "text/plain", "resume");
}

static esp_err_t audio_volume_handler(httpd_req_t *req)
{
    char q[32];
    char val[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "val", val, sizeof(val));
    }
    int v = atoi(val);
    audio_player_set_volume(v);
    return web_ui_send_ok(req, "text/plain", "vol set");
}

static esp_err_t audio_seek_handler(httpd_req_t *req)
{
    char q[64];
    char pos[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pos required");
    }
    httpd_query_key_value(q, "pos", pos, sizeof(pos));
    int ms = atoi(pos);
    if (ms < 0) ms = 0;
    esp_err_t err = audio_player_seek((uint32_t)ms);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "seek failed");
    }
    return web_ui_send_ok(req, "text/plain", "seek");
}

static esp_err_t publish_handler(httpd_req_t *req)
{
    char query[160];
    char topic_enc[96] = {0};
    char payload_enc[96] = {0};
    char topic[96] = {0};
    char payload[96] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "topic", topic_enc, sizeof(topic_enc));
        httpd_query_key_value(query, "payload", payload_enc, sizeof(payload_enc));
    }
    web_ui_url_decode(topic, sizeof(topic), topic_enc);
    web_ui_url_decode(payload, sizeof(payload), payload_enc);
    if (topic[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "topic required");
    }
    #if WEB_UI_DEBUG
    ESP_LOGI(TAG, "publish request topic='%s' payload='%s'",
             topic, payload[0] ? payload : "<none>");
#endif
    event_bus_message_t msg = {.type = EVENT_WEB_COMMAND};
    strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
    strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    event_bus_post(&msg, pdMS_TO_TICKS(50));
    return web_ui_send_ok(req, "text/plain", "sent");
}

static esp_err_t ap_stop_handler(httpd_req_t *req)
{
    network_stop_ap();
    return web_ui_send_ok(req, "text/plain", "ap stopped");
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return web_ui_send_ok(req, "text/html", web_ui_get_index_html());
}

static esp_err_t start_httpd(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 40; // many handlers registered
    config.max_open_sockets = 12;  // allow up to a dozen concurrent clients
    config.backlog_conn = 12;
    config.lru_purge_enable = false; // avoid extra socket churn
    config.keep_alive_enable = false; // close connections immediately
    config.stack_size = 16384; // avoid stack overflow with larger handlers/pages
    #if WEB_UI_DEBUG
    ESP_LOGI(TAG, "starting httpd: uri=%d sockets=%d backlog=%d keepalive=%d free_int=%u",
             config.max_uri_handlers, config.max_open_sockets, config.backlog_conn, config.keep_alive_enable,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(web_ui_devices_register_assets(s_server), TAG, "devices assets register failed");

    static web_route_t route_root = {.fn = root_get_handler, .redirect_on_fail = true};
    static web_route_t route_ping = {.fn = ping_handler, .redirect_on_fail = false};
    static web_route_t route_status = {.fn = status_handler, .redirect_on_fail = false};
    static web_route_t route_wifi = {.fn = wifi_config_handler, .redirect_on_fail = false};
    static web_route_t route_mqtt = {.fn = mqtt_config_handler, .redirect_on_fail = false};
    static web_route_t route_mqtt_users = {.fn = mqtt_users_handler, .redirect_on_fail = false};
    static web_route_t route_wifi_scan = {.fn = wifi_scan_handler, .redirect_on_fail = false};
    static web_route_t route_ap_stop = {.fn = ap_stop_handler, .redirect_on_fail = false};
    static web_route_t route_play = {.fn = audio_play_handler, .redirect_on_fail = false};
    static web_route_t route_stop = {.fn = audio_stop_handler, .redirect_on_fail = false};
    static web_route_t route_pause = {.fn = audio_pause_handler, .redirect_on_fail = false};
    static web_route_t route_resume = {.fn = audio_resume_handler, .redirect_on_fail = false};
    static web_route_t route_vol = {.fn = audio_volume_handler, .redirect_on_fail = false};
    static web_route_t route_seek = {.fn = audio_seek_handler, .redirect_on_fail = false};
    static web_route_t route_pub = {.fn = publish_handler, .redirect_on_fail = false};
    static web_route_t route_files = {.fn = files_handler, .redirect_on_fail = false};
    static web_route_t route_devices_cfg = {.fn = devices_config_handler, .redirect_on_fail = false};
    static web_route_t route_devices_apply = {.fn = devices_apply_handler, .redirect_on_fail = false};
    static web_route_t route_devices_run = {.fn = devices_run_handler, .redirect_on_fail = false};
    static web_route_t route_profile_create = {.fn = devices_profile_create_handler, .redirect_on_fail = false};
    static web_route_t route_profile_delete = {.fn = devices_profile_delete_handler, .redirect_on_fail = false};
    static web_route_t route_profile_rename = {.fn = devices_profile_rename_handler, .redirect_on_fail = false};
    static web_route_t route_profile_activate = {.fn = devices_profile_activate_handler, .redirect_on_fail = false};
    static web_route_t route_variables = {.fn = devices_variables_handler, .redirect_on_fail = false};
    static web_route_t route_templates = {.fn = devices_templates_handler, .redirect_on_fail = false};
    static web_route_t route_auth_password = {.fn = auth_password_handler, .redirect_on_fail = false};
    static web_route_t route_logout = {.fn = auth_logout_handler, .redirect_on_fail = false};

    ESP_RETURN_ON_ERROR(register_public_route("/login", HTTP_GET, login_page_handler), TAG, "register login");
    ESP_RETURN_ON_ERROR(register_public_route("/api/auth/login", HTTP_POST, auth_login_handler), TAG, "register auth login");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/auth/logout", HTTP_POST, &route_logout), TAG, "register logout");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/auth/password", HTTP_POST, &route_auth_password), TAG, "register auth password");
    ESP_RETURN_ON_ERROR(register_guarded_route("/", HTTP_GET, &route_root), TAG, "register root");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/status", HTTP_GET, &route_status), TAG, "register status");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/ping", HTTP_GET, &route_ping), TAG, "register ping");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/config/wifi", HTTP_GET, &route_wifi), TAG, "register wifi cfg");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/config/mqtt", HTTP_GET, &route_mqtt), TAG, "register mqtt cfg");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/config/mqtt_users", HTTP_POST, &route_mqtt_users), TAG, "register mqtt users");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/wifi/scan", HTTP_GET, &route_wifi_scan), TAG, "register wifi scan");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/ap/stop", HTTP_GET, &route_ap_stop), TAG, "register ap stop");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/audio/play", HTTP_GET, &route_play), TAG, "register audio play");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/audio/stop", HTTP_GET, &route_stop), TAG, "register audio stop");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/audio/pause", HTTP_GET, &route_pause), TAG, "register audio pause");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/audio/resume", HTTP_GET, &route_resume), TAG, "register audio resume");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/audio/volume", HTTP_GET, &route_vol), TAG, "register audio volume");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/audio/seek", HTTP_GET, &route_seek), TAG, "register audio seek");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/publish", HTTP_GET, &route_pub), TAG, "register publish");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/files", HTTP_GET, &route_files), TAG, "register files");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/config", HTTP_GET, &route_devices_cfg), TAG, "register devices cfg");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/apply", HTTP_POST, &route_devices_apply), TAG, "register devices apply");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/run", HTTP_GET, &route_devices_run), TAG, "register devices run");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/profile/create", HTTP_POST, &route_profile_create), TAG, "register profile create");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/profile/delete", HTTP_POST, &route_profile_delete), TAG, "register profile delete");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/profile/rename", HTTP_POST, &route_profile_rename), TAG, "register profile rename");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/profile/activate", HTTP_POST, &route_profile_activate), TAG, "register profile activate");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/variables", HTTP_GET, &route_variables), TAG, "register variables");
    ESP_RETURN_ON_ERROR(register_guarded_route("/api/devices/templates", HTTP_GET, &route_templates), TAG, "register templates");
    return ESP_OK;
}

esp_err_t web_ui_init(void)
{
    web_sessions_init();
    web_auth_start_reset_monitor();
    return ESP_OK;
}

esp_err_t web_ui_start(void)
{
    ESP_RETURN_ON_ERROR(start_httpd(), TAG, "start httpd failed");
    #if WEB_UI_DEBUG
    ESP_LOGI(TAG, "web ui started on port 80");
#endif
    return ESP_OK;
}


static esp_err_t devices_templates_handler(httpd_req_t *req)
{
    size_t count = 0;
    const dm_template_descriptor_t *templates = dm_template_registry_get_all(&count);
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    for (size_t i = 0; i < count; ++i) {
        const dm_template_descriptor_t *tpl = &templates[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        }
        cJSON_AddStringToObject(obj, "id", tpl->id);
        cJSON_AddStringToObject(obj, "label", tpl->label);
        cJSON_AddStringToObject(obj, "description", tpl->description);
        cJSON_AddStringToObject(obj, "type", dm_template_type_to_string(tpl->type));
        cJSON_AddItemToArray(root, obj);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return res;
}
