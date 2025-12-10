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

#include "web_ui_page.h"
#include "web_ui_utils.h"
#include "web_ui_devices.h"

static const char *TAG = "web_ui";
static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_scan_mutex = NULL;

static esp_err_t devices_templates_handler(httpd_req_t *req);
static char *build_uid_monitor_json(void);
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
    const device_manager_config_t *cfg = device_manager_get();
    if (!cfg) {
        return dup_empty_json_array();
    }
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return dup_empty_json_array();
    }
    for (uint8_t i = 0; i < cfg->device_count && i < DEVICE_MANAGER_MAX_DEVICES; ++i) {
        const device_descriptor_t *dev = &cfg->devices[i];
        if (!dev->template_assigned || dev->template_config.type != DM_TEMPLATE_TYPE_UID) {
            continue;
        }
        cJSON *dev_obj = cJSON_CreateObject();
        if (!dev_obj) {
            cJSON_Delete(root);
            return dup_empty_json_array();
        }
        cJSON_AddStringToObject(dev_obj, "id", dev->id);
        cJSON_AddStringToObject(dev_obj, "name", dev->display_name[0] ? dev->display_name : dev->id);
        cJSON *slot_arr = cJSON_AddArrayToObject(dev_obj, "slots");
        if (!slot_arr) {
            cJSON_Delete(root);
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
    if (!printed) {
        return dup_empty_json_array();
    }
    return printed;
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
    const char *fmt =
        "{\"wifi\":{\"ssid\":\"%s\",\"host\":\"%s\",\"sta_ip\":\"%s\",\"ap\":%s},"
        "\"mqtt\":{\"id\":\"%s\",\"port\":%d,\"keepalive\":%d},"
        "\"audio\":{\"volume\":%d,\"playing\":%s,\"paused\":%s,\"progress\":%d,\"pos_ms\":%d,\"dur_ms\":%d,"
        "\"bitrate\":%d,\"path\":\"%s\",\"message\":\"%s\",\"fmt\":%d},"
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
                          sd_ok ? "true" : "false",
                          (unsigned long long)sd_total,
                          (unsigned long long)sd_free,
                          stats.total,
                          uid_json ? uid_json : "[]");
    if (needed < 0) {
        if (uid_json) {
            free(uid_json);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status format err");
    }
    size_t buf_len = (size_t)needed + 1;
    char *buf = heap_caps_malloc(buf_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        if (uid_json) {
            free(uid_json);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    snprintf(buf, buf_len, fmt,
             cfg->wifi.ssid, cfg->wifi.hostname, ip_buf, network_is_ap_mode() ? "true" : "false",
             cfg->mqtt.broker_id, cfg->mqtt.port, cfg->mqtt.keepalive_seconds,
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

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t ping = {.uri = "/api/ping", .method = HTTP_GET, .handler = ping_handler};
    httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler};
    httpd_uri_t wifi = {.uri = "/api/config/wifi", .method = HTTP_GET, .handler = wifi_config_handler};
    httpd_uri_t mqtt = {.uri = "/api/config/mqtt", .method = HTTP_GET, .handler = mqtt_config_handler};
    httpd_uri_t wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler};
    httpd_uri_t ap_stop = {.uri = "/api/ap/stop", .method = HTTP_GET, .handler = ap_stop_handler};
    httpd_uri_t play = {.uri = "/api/audio/play", .method = HTTP_GET, .handler = audio_play_handler};
    httpd_uri_t stop = {.uri = "/api/audio/stop", .method = HTTP_GET, .handler = audio_stop_handler};
    httpd_uri_t pause = {.uri = "/api/audio/pause", .method = HTTP_GET, .handler = audio_pause_handler};
    httpd_uri_t resume = {.uri = "/api/audio/resume", .method = HTTP_GET, .handler = audio_resume_handler};
    httpd_uri_t vol = {.uri = "/api/audio/volume", .method = HTTP_GET, .handler = audio_volume_handler};
    httpd_uri_t seek = {.uri = "/api/audio/seek", .method = HTTP_GET, .handler = audio_seek_handler};
    httpd_uri_t pub = {.uri = "/api/publish", .method = HTTP_GET, .handler = publish_handler};
    httpd_uri_t files = {.uri = "/api/files", .method = HTTP_GET, .handler = files_handler};
    httpd_uri_t devices_cfg = {.uri = "/api/devices/config", .method = HTTP_GET, .handler = devices_config_handler};
    httpd_uri_t devices_apply = {.uri = "/api/devices/apply", .method = HTTP_POST, .handler = devices_apply_handler};
    httpd_uri_t devices_run = {.uri = "/api/devices/run", .method = HTTP_GET, .handler = devices_run_handler};
    httpd_uri_t devices_profile_create = {.uri = "/api/devices/profile/create", .method = HTTP_POST, .handler = devices_profile_create_handler};
    httpd_uri_t devices_profile_delete = {.uri = "/api/devices/profile/delete", .method = HTTP_POST, .handler = devices_profile_delete_handler};
    httpd_uri_t devices_profile_rename = {.uri = "/api/devices/profile/rename", .method = HTTP_POST, .handler = devices_profile_rename_handler};
    httpd_uri_t devices_profile_activate = {.uri = "/api/devices/profile/activate", .method = HTTP_POST, .handler = devices_profile_activate_handler};
    httpd_uri_t devices_variables = {.uri = "/api/devices/variables", .method = HTTP_GET, .handler = devices_variables_handler};
    httpd_uri_t devices_templates = {.uri = "/api/devices/templates", .method = HTTP_GET, .handler = devices_templates_handler};

    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &ping);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &wifi);
    httpd_register_uri_handler(s_server, &mqtt);
    httpd_register_uri_handler(s_server, &wifi_scan);
    httpd_register_uri_handler(s_server, &ap_stop);
    httpd_register_uri_handler(s_server, &play);
    httpd_register_uri_handler(s_server, &stop);
    httpd_register_uri_handler(s_server, &pause);
    httpd_register_uri_handler(s_server, &resume);
    httpd_register_uri_handler(s_server, &vol);
    httpd_register_uri_handler(s_server, &seek);
    httpd_register_uri_handler(s_server, &pub);
    httpd_register_uri_handler(s_server, &files);
    httpd_register_uri_handler(s_server, &devices_cfg);
    httpd_register_uri_handler(s_server, &devices_apply);
    httpd_register_uri_handler(s_server, &devices_run);
    httpd_register_uri_handler(s_server, &devices_profile_create);
    httpd_register_uri_handler(s_server, &devices_profile_delete);
    httpd_register_uri_handler(s_server, &devices_profile_rename);
    httpd_register_uri_handler(s_server, &devices_profile_activate);
    httpd_register_uri_handler(s_server, &devices_variables);
    httpd_register_uri_handler(s_server, &devices_templates);

    return ESP_OK;
}

esp_err_t web_ui_init(void)
{
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
