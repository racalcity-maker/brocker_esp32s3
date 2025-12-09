#include "web_ui_pictures.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "audio_player.h"
#include "automation_engine.h"
#include "device_manager.h"
#include "mqtt_core.h"
#include "nvs.h"

#include "web_ui_utils.h"

#define PIC_DEVICES 5
#define PIC_UIDS 10
#define PIC_FORCE_DELAY_US (700 * 1000)

typedef struct {
    char uids[PIC_UIDS][16];
    uint8_t count;
    char last_uid[16];
} pic_device_cfg_t;

typedef struct {
    pic_device_cfg_t dev[PIC_DEVICES];
    char track_ok[96];
    char track_fail[96];
} pic_cfg_t;

static const char *TAG = "web_ui";
static pic_cfg_t s_pic_cfg;
static bool s_pic_slot_seen[PIC_DEVICES];
static SemaphoreHandle_t s_pic_mutex;
static esp_timer_handle_t s_pic_force_timer;

static bool pic_run_check(void);

static void pic_lock(void)
{
    if (s_pic_mutex) {
        xSemaphoreTake(s_pic_mutex, portMAX_DELAY);
    }
}

static void pic_unlock(void)
{
    if (s_pic_mutex) {
        xSemaphoreGive(s_pic_mutex);
    }
}

static void parse_uids_csv(const char *csv, char out[][16], uint8_t *out_count)
{
    if (!csv || !out || !out_count) {
        return;
    }
    uint8_t cnt = 0;
    const char *p = csv;
    while (*p && cnt < PIC_UIDS) {
        while (*p == ',' || *p == ' ') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        char buf[16] = {0};
        int i = 0;
        while (*p && *p != ',' && i < (int)sizeof(buf) - 1) {
            buf[i++] = *p++;
        }
        strncpy(out[cnt], buf, sizeof(out[cnt]) - 1);
        out[cnt][sizeof(out[cnt]) - 1] = 0;
        cnt++;
        if (*p == ',') {
            p++;
        }
    }
    *out_count = cnt;
}

static void pic_load_defaults(pic_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
}

static esp_err_t pic_load(pic_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("piccfg", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pic load: no nvs namespace (%s)", esp_err_to_name(err));
        pic_load_defaults(cfg);
        return err;
    }
    size_t sz = sizeof(*cfg);
    err = nvs_get_blob(h, "cfg", cfg, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(*cfg)) {
        ESP_LOGW(TAG, "pic load: get_blob failed %s sz=%u expected=%u", esp_err_to_name(err), (unsigned)sz, (unsigned)sizeof(*cfg));
        pic_load_defaults(cfg);
        return err;
    }
    return ESP_OK;
}

static esp_err_t pic_save(const pic_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("piccfg", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pic save: open failed %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    } else {
        ESP_LOGE(TAG, "pic save: set_blob failed %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}

static bool pic_uid_allowed(int idx, const char *uid)
{
    if (idx < 0 || idx >= PIC_DEVICES || !uid || uid[0] == '\0') {
        return false;
    }
    const pic_device_cfg_t *d = &s_pic_cfg.dev[idx];
    for (int i = 0; i < d->count; ++i) {
        if (strcmp(d->uids[i], uid) == 0) {
            return true;
        }
    }
    return false;
}

static bool pic_all_match_locked(void)
{
    bool ok = true;
    int configured_devices = 0;
    int missing_scan = 0;
    int missing_uid = 0;
    int invalid_uid = 0;
    for (int i = 0; i < PIC_DEVICES; ++i) {
        const pic_device_cfg_t *d = &s_pic_cfg.dev[i];
        if (d->count == 0) {
            continue;
        }
        configured_devices++;
        bool seen = s_pic_slot_seen[i];
        if (!seen) {
            missing_scan++;
            ok = false;
            continue;
        }
        if (!d->last_uid[0]) {
            missing_uid++;
            ok = false;
            continue;
        }
        bool allowed = pic_uid_allowed(i, d->last_uid);
        if (!allowed) {
            invalid_uid++;
            ok = false;
        }
    }
    if (configured_devices == 0) {
        ESP_LOGW(TAG, "check: no active devices configured");
        return false;
    }
    int active = configured_devices - missing_scan;
    if (ok) {
        ESP_LOGI(TAG, "pictures summary: configured=%d active=%d missing_scan=%d missing_uid=%d invalid=%d -> OK",
                 configured_devices, active, missing_scan, missing_uid, invalid_uid);
    } else {
        ESP_LOGW(TAG, "pictures summary: configured=%d active=%d missing_scan=%d missing_uid=%d invalid=%d -> FAIL",
                 configured_devices, active, missing_scan, missing_uid, invalid_uid);
    }
    return ok;
}

static bool pic_all_match(void)
{
    bool ok;
    pic_lock();
    ok = pic_all_match_locked();
    pic_unlock();
    return ok;
}

static void pictures_run_fallback(bool ok, const char *track)
{
    const char *light_topic = ok ? "pictures/lightsgreen" : "pictures/lightsred";
    const char *payload = ok ? "ok" : "fail";
    mqtt_core_publish(light_topic, payload);
    if (track && track[0]) {
        mqtt_core_publish("robot/speak", track);
        audio_player_play(track);
    }
}

static void pictures_trigger_actions(bool ok, const char *track)
{
    const char *scenario = ok ? DEVICE_MANAGER_SCENARIO_PICTURES_OK : DEVICE_MANAGER_SCENARIO_PICTURES_FAIL;
    ESP_LOGI(TAG, "result: %s -> trigger scenario %s", ok ? "OK" : "FAIL", scenario);
    esp_err_t err = automation_engine_trigger(DEVICE_MANAGER_DEVICE_ID_PICTURES, scenario);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "automation trigger failed for pictures (%s), fallback", esp_err_to_name(err));
        pictures_run_fallback(ok, track);
    }
}

static void pic_handle_result(bool ok)
{
    static int64_t s_last_result_ms = 0;
    static bool s_last_result_valid = false;
    static bool s_last_result_ok = false;

    int64_t now = esp_timer_get_time() / 1000;
    if (s_last_result_valid) {
        int64_t dt = now - s_last_result_ms;
        if (dt >= 0 && dt < 800 && ok == s_last_result_ok) {
            ESP_LOGI(TAG, "result: duplicate %s within %lld ms, skip", ok ? "OK" : "FAIL", (long long)dt);
            return;
        }
    }
    s_last_result_valid = true;
    s_last_result_ms = now;
    s_last_result_ok = ok;

    char robot_path[96] = {0};
    char local_track[96] = {0};
    pic_lock();
    const char *robot_src = ok ? s_pic_cfg.track_ok : s_pic_cfg.track_fail;
    const char *local_src = ok ? s_pic_cfg.track_ok : s_pic_cfg.track_fail;
    strncpy(robot_path, robot_src, sizeof(robot_path) - 1);
    strncpy(local_track, local_src, sizeof(local_track) - 1);
    pic_unlock();
    automation_engine_set_variable("pictures.result_state", ok ? "ok" : "fail");
    if (local_track[0]) {
        automation_engine_set_variable("pictures.result_track", local_track);
    } else {
        automation_engine_clear_variable("pictures.result_track");
    }
    pictures_trigger_actions(ok, local_track);
}

static void pic_handle_scan(int idx, const char *uid)
{
    if (idx < 0 || idx >= PIC_DEVICES || !uid) {
        ESP_LOGW(TAG, "scan: bad idx=%d or uid=NULL", idx);
        return;
    }
    ESP_LOGI(TAG, "scan: dev=%d, uid='%s'", idx + 1, uid);
    pic_lock();
    strncpy(s_pic_cfg.dev[idx].last_uid, uid, sizeof(s_pic_cfg.dev[idx].last_uid) - 1);
    s_pic_cfg.dev[idx].last_uid[sizeof(s_pic_cfg.dev[idx].last_uid) - 1] = 0;
    s_pic_slot_seen[idx] = true;
    pic_unlock();
    bool ok = pic_all_match();
    ESP_LOGI(TAG, "scan: after update -> all_match=%d", ok);
}

static bool pic_run_check(void)
{
    if (s_pic_force_timer) {
        esp_timer_stop(s_pic_force_timer);
    }
    bool ok = pic_all_match();
    ESP_LOGI(TAG, "pictures check -> ok=%d", ok);
    pic_handle_result(ok);
    return ok;
}

static void pic_force_timer_cb(void *arg)
{
    (void)arg;
    pic_run_check();
}

static void pic_start_force_cycle(void)
{
    pic_lock();
    memset(s_pic_slot_seen, 0, sizeof(s_pic_slot_seen));
    pic_unlock();
    if (s_pic_force_timer) {
        esp_timer_stop(s_pic_force_timer);
        esp_timer_start_once(s_pic_force_timer, PIC_FORCE_DELAY_US);
    }
}

void web_ui_pictures_request_force_cycle(void)
{
    pic_start_force_cycle();
    mqtt_core_publish("pictures/cmd/scan", "scan");
}

esp_err_t web_ui_pictures_init(void)
{
    pic_load(&s_pic_cfg);
    device_manager_update_pictures_tracks(s_pic_cfg.track_ok, s_pic_cfg.track_fail);
    memset(s_pic_slot_seen, 0, sizeof(s_pic_slot_seen));
    if (!s_pic_mutex) {
        s_pic_mutex = xSemaphoreCreateMutex();
    }
    if (!s_pic_mutex) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_pic_force_timer) {
        const esp_timer_create_args_t args = {
            .callback = pic_force_timer_cb,
            .name = "pic_force",
        };
        esp_err_t err = esp_timer_create(&args, &s_pic_force_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t pictures_config_handler(httpd_req_t *req)
{
    char *resp = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    pic_cfg_t snapshot = {0};
    pic_lock();
    snapshot = s_pic_cfg;
    pic_unlock();
    size_t len = 0;
#define APPEND(fmt, ...) do { int w = snprintf(resp + len, 4096 - len, fmt, ##__VA_ARGS__); if (w < 0 || (size_t)w >= 4096 - len) { free(resp); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "resp too long"); } len += (size_t)w; } while (0)
    APPEND("{\"ok\":\"%s\",\"fail\":\"%s\",\"devices\":[", snapshot.track_ok, snapshot.track_fail);
    for (int i = 0; i < PIC_DEVICES; ++i) {
        APPEND("%s{\"idx\":%d,\"last\":\"%s\",\"uids\":[", (i == 0 ? "" : ","), i + 1, snapshot.dev[i].last_uid);
        for (int j = 0; j < snapshot.dev[i].count; ++j) {
            APPEND("%s\"%s\"", (j == 0 ? "" : ","), snapshot.dev[i].uids[j]);
        }
        APPEND("]}");
    }
    APPEND("]}");
#undef APPEND
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, resp, len);
    free(resp);
    return r;
}

static esp_err_t pictures_status_handler(httpd_req_t *req)
{
    char resp[PIC_DEVICES * 40 + 16];
    size_t len = 0;
    pic_cfg_t snapshot = {0};
    bool seen[PIC_DEVICES] = {0};
    pic_lock();
    snapshot = s_pic_cfg;
    memcpy(seen, s_pic_slot_seen, sizeof(seen));
    pic_unlock();
    len += snprintf(resp + len, sizeof(resp) - len, "{\"devices\":[");
    for (int i = 0; i < PIC_DEVICES; ++i) {
        const char *last = snapshot.dev[i].last_uid;
        bool active = seen[i];
        len += snprintf(resp + len, sizeof(resp) - len,
                        "%s{\"idx\":%d,\"last\":\"%s\",\"active\":%s}",
                        (i == 0 ? "" : ","), i + 1,
                        last ? last : "",
                        active ? "true" : "false");
    }
    len += snprintf(resp + len, sizeof(resp) - len, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len);
}

static esp_err_t pictures_device_save_handler(httpd_req_t *req)
{
    char q[512];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "query required");
    }

    char idxs[8] = {0};
    char uids_enc[256] = {0};
    char uids_dec[256] = {0};

    httpd_query_key_value(q, "idx", idxs, sizeof(idxs));
    httpd_query_key_value(q, "uids", uids_enc, sizeof(uids_enc));

    int idx = atoi(idxs) - 1;
    if (idx < 0 || idx >= PIC_DEVICES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad idx");
    }

    web_ui_url_decode(uids_dec, sizeof(uids_dec), uids_enc);

    pic_lock();
    parse_uids_csv(uids_dec, s_pic_cfg.dev[idx].uids, &s_pic_cfg.dev[idx].count);
    s_pic_slot_seen[idx] = false;
    s_pic_cfg.dev[idx].last_uid[0] = 0;
    esp_err_t err = pic_save(&s_pic_cfg);
    pic_unlock();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
    }
    httpd_resp_set_hdr(req, "Connection", "close");
    return web_ui_send_ok(req, "text/plain", "saved");
}

static esp_err_t pictures_tracks_save_handler(httpd_req_t *req)
{
    char q[384];
    char ok[96] = {0}, fail[96] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "ok", ok, sizeof(ok));
        httpd_query_key_value(q, "fail", fail, sizeof(fail));
    }
    char ok_dec[96] = {0}, fail_dec[96] = {0};
    web_ui_url_decode(ok_dec, sizeof(ok_dec), ok);
    web_ui_url_decode(fail_dec, sizeof(fail_dec), fail);
    char ok_copy[96] = {0};
    char fail_copy[96] = {0};
    pic_lock();
    if (ok_dec[0]) {
        strncpy(s_pic_cfg.track_ok, ok_dec, sizeof(s_pic_cfg.track_ok) - 1);
    }
    if (fail_dec[0]) {
        strncpy(s_pic_cfg.track_fail, fail_dec, sizeof(s_pic_cfg.track_fail) - 1);
    }
    esp_err_t err = pic_save(&s_pic_cfg);
    strncpy(ok_copy, s_pic_cfg.track_ok, sizeof(ok_copy) - 1);
    strncpy(fail_copy, s_pic_cfg.track_fail, sizeof(fail_copy) - 1);
    pic_unlock();
    ESP_LOGI(TAG, "pictures tracks saved ok='%s' fail='%s' err=%s", ok_copy, fail_copy, esp_err_to_name(err));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
    }
    device_manager_update_pictures_tracks(s_pic_cfg.track_ok, s_pic_cfg.track_fail);
    httpd_resp_set_hdr(req, "Connection", "close");
    return web_ui_send_ok(req, "text/plain", "tracks saved");
}

static esp_err_t pictures_addmode_handler(httpd_req_t *req)
{
    char q[64];
    char mode[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "mode", mode, sizeof(mode));
    }
    const char *m = (strcmp(mode, "on") == 0) ? "on" : "off";
    mqtt_core_publish("pictures/add_mode", m);
    return web_ui_send_ok(req, "text/plain", "add mode sent");
}

static esp_err_t pictures_scan_handler(httpd_req_t *req)
{
    web_ui_pictures_request_force_cycle();
    return web_ui_send_ok(req, "application/json", "{\"ok\":true}");
}

static esp_err_t pictures_check_handler(httpd_req_t *req)
{
    bool ok = web_ui_pictures_handle_check();
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
    return web_ui_send_ok(req, "application/json", resp);
}

esp_err_t web_ui_pictures_register(httpd_handle_t server)
{
    httpd_uri_t pic_cfg = {.uri = "/api/pictures/config", .method = HTTP_GET, .handler = pictures_config_handler};
    httpd_uri_t pic_status = {.uri = "/api/pictures/status", .method = HTTP_GET, .handler = pictures_status_handler};
    httpd_uri_t pic_dev = {.uri = "/api/pictures/device/save", .method = HTTP_GET, .handler = pictures_device_save_handler};
    httpd_uri_t pic_tracks = {.uri = "/api/pictures/tracks/save", .method = HTTP_GET, .handler = pictures_tracks_save_handler};
    httpd_uri_t pic_addmode = {.uri = "/api/pictures/addmode", .method = HTTP_GET, .handler = pictures_addmode_handler};
    httpd_uri_t pic_scan = {.uri = "/api/pictures/scan", .method = HTTP_GET, .handler = pictures_scan_handler};
    httpd_uri_t pic_check = {.uri = "/api/pictures/check", .method = HTTP_GET, .handler = pictures_check_handler};
    httpd_register_uri_handler(server, &pic_cfg);
    httpd_register_uri_handler(server, &pic_status);
    httpd_register_uri_handler(server, &pic_dev);
    httpd_register_uri_handler(server, &pic_tracks);
    httpd_register_uri_handler(server, &pic_addmode);
    httpd_register_uri_handler(server, &pic_scan);
    httpd_register_uri_handler(server, &pic_check);
    return ESP_OK;
}

void web_ui_pictures_handle_scan(int idx, const char *uid)
{
    pic_handle_scan(idx, uid);
}

bool web_ui_pictures_handle_check(void)
{
    return pic_run_check();
}
