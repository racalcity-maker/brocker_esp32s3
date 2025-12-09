#include "web_ui_laser.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_check.h"
#include "nvs.h"
#include "automation_engine.h"
#include "device_manager.h"
#include "mqtt_core.h"
#include "audio_player.h"

#include "web_ui_utils.h"

#define LASER_TIMEOUT_US (2000000) // consider laser off if no heartbeat for 2s
#define LASER_ROBOT_INTERVAL_MS (10000)
#define LASER_ROBOT_MAX_TRACKS 32

typedef struct {
    char track_hold[96];
    char track_relay[96];
    int hold_seconds;
    bool cumulative;
    bool enabled;
} laser_cfg_t;

static const char *TAG = "web_ui";
static laser_cfg_t s_laser_cfg;
static SemaphoreHandle_t s_laser_mutex;

static bool s_laser_present;
static bool s_laser_playing;
static bool s_relay_triggered;
static bool s_hold_completed;
static int64_t s_last_heartbeat_ms;
static int64_t s_hold_total_ms;
static int64_t s_hold_streak_ms;
static int64_t s_robot_last_ms;
static esp_timer_handle_t s_laser_timeout;

static char s_robot_tracks[LASER_ROBOT_MAX_TRACKS][96];
static int s_robot_track_count;
static bool s_robot_scanned;

static void laser_trigger_actions(const char *track);

static void laser_lock(void)
{
    if (s_laser_mutex) {
        xSemaphoreTake(s_laser_mutex, portMAX_DELAY);
    }
}

static void laser_unlock(void)
{
    if (s_laser_mutex) {
        xSemaphoreGive(s_laser_mutex);
    }
}

static void laser_load_defaults(laser_cfg_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->hold_seconds = 20;
    cfg->cumulative = true;
    cfg->enabled = true;
}

static esp_err_t laser_load(laser_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("lasercfg", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "laser load: no nvs namespace (%s)", esp_err_to_name(err));
        laser_load_defaults(cfg);
        return err;
    }
    size_t sz = sizeof(*cfg);
    err = nvs_get_blob(h, "cfg", cfg, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(*cfg)) {
        ESP_LOGW(TAG, "laser load: get_blob failed %s sz=%u expected=%u",
                 esp_err_to_name(err), (unsigned)sz, (unsigned)sizeof(*cfg));
        laser_load_defaults(cfg);
        return err;
    }
    return ESP_OK;
}

static esp_err_t laser_save(const laser_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("lasercfg", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "laser save: open failed %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    } else {
        ESP_LOGE(TAG, "laser save: set_blob failed %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool is_audio_ext(const char *ext)
{
    if (!ext) {
        return false;
    }
    return strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".ogg") == 0;
}

static void laser_scan_robot_tracks(void)
{
    s_robot_track_count = 0;
    s_robot_scanned = true;
    if (audio_player_mount_sd() != ESP_OK) {
        return;
    }
    DIR *d = opendir("/sdcard/laser");
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_robot_track_count < LASER_ROBOT_MAX_TRACKS) {
        const char *n = ent->d_name;
        size_t nlen = strlen(n);
        if (nlen < 4) {
            continue;
        }
        const char *ext = n + nlen - 4;
        if (!is_audio_ext(ext)) {
            continue;
        }
        const size_t prefix_len = strlen("/sdcard/laser/");
        const size_t buf_sz = sizeof(s_robot_tracks[s_robot_track_count]);
        if (buf_sz <= prefix_len + 1) {
            continue;
        }
        size_t max_name = buf_sz - prefix_len - 1;
        size_t copy_len = strnlen(n, max_name);
        snprintf(s_robot_tracks[s_robot_track_count], buf_sz, "/sdcard/laser/%.*s", (int)copy_len, n);
        s_robot_track_count++;
    }
    closedir(d);
}

static const char *laser_random_robot_track(void)
{
    if (!s_robot_scanned) {
        laser_scan_robot_tracks();
    }
    if (s_robot_track_count == 0) {
        return NULL;
    }
    uint32_t r = esp_random();
    int idx = (int)(r % s_robot_track_count);
    return s_robot_tracks[idx];
}

static void laser_stop_audio(void)
{
    if (!s_laser_playing) {
        return;
    }
    audio_player_pause();
}

static void laser_start_audio(void)
{
    if (!s_laser_cfg.enabled) {
        return;
    }
    if (s_hold_completed) {
        return;
    }
    if (s_laser_playing) {
        audio_player_resume();
        return;
    }
    if (s_laser_cfg.track_hold[0]) {
        audio_player_play(s_laser_cfg.track_hold);
        s_laser_playing = true;
    }
}

static void laser_trigger_relay(void)
{
    s_relay_triggered = true;
    s_hold_completed = true;
    ESP_LOGI(TAG, "relay trigger: executing laser scenario");
    audio_player_stop();
    s_laser_playing = false;
    if (s_laser_cfg.track_relay[0]) {
        automation_engine_set_variable("laser.result_track", s_laser_cfg.track_relay);
    } else {
        automation_engine_clear_variable("laser.result_track");
    }
    laser_trigger_actions(s_laser_cfg.track_relay);
}

static void laser_maybe_send_robot(int64_t now)
{
    if (!s_laser_cfg.enabled || !s_laser_present) {
        return;
    }
    if (now - s_robot_last_ms < LASER_ROBOT_INTERVAL_MS) {
        return;
    }
    const char *path = laser_random_robot_track();
    if (!path) {
        return;
    }
    ESP_LOGI(TAG, "robot speak: publishing robot/laser/captured -> %s", path);
    mqtt_core_publish("robot/laser/captured", path);
    s_robot_last_ms = now;
}

static void laser_handle_timeout(void *arg)
{
    (void)arg;
    laser_lock();
    s_laser_present = false;
    s_last_heartbeat_ms = 0;
    s_hold_streak_ms = 0;
    if (!s_laser_cfg.cumulative) {
        s_hold_total_ms = 0;
        s_relay_triggered = false;
        s_hold_completed = false;
    }
    laser_stop_audio();
    laser_unlock();
}

static void laser_arm_timeout(void)
{
    if (!s_laser_timeout) {
        esp_timer_create_args_t args = {
            .callback = laser_handle_timeout,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "laser_timeout",
        };
        if (esp_timer_create(&args, &s_laser_timeout) != ESP_OK) {
            return;
        }
    }
    if (esp_timer_is_active(s_laser_timeout)) {
        esp_timer_stop(s_laser_timeout);
    }
    esp_timer_start_once(s_laser_timeout, LASER_TIMEOUT_US);
}

static void laser_maybe_trigger_relay(void)
{
    if (s_relay_triggered) {
        return;
    }
    int64_t base_ms = s_laser_cfg.cumulative ? s_hold_total_ms : s_hold_streak_ms;
    if (s_laser_cfg.hold_seconds > 0 && base_ms >= ((int64_t)s_laser_cfg.hold_seconds) * 1000) {
        laser_trigger_relay();
    }
}

static void laser_run_fallback(const char *track)
{
    mqtt_core_publish("relay/relayOn", "on");
    mqtt_core_publish("robot/laser/relayOn.mp3", track && track[0] ? track : "/sdcard/laser/relayOn.mp3");
    if (track && track[0]) {
        audio_player_play(track);
    }
}

static void laser_trigger_actions(const char *track)
{
    ESP_LOGI(TAG, "relay trigger -> scenario %s", DEVICE_MANAGER_SCENARIO_LASER_TRIGGER);
    esp_err_t err = automation_engine_trigger(DEVICE_MANAGER_DEVICE_ID_LASER,
                                              DEVICE_MANAGER_SCENARIO_LASER_TRIGGER);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "automation trigger failed for laser (%s), fallback", esp_err_to_name(err));
        laser_run_fallback(track);
    }
}

static void laser_set_enabled_locked(bool en)
{
    if (s_laser_cfg.enabled == en) {
        return;
    }
    s_laser_cfg.enabled = en;
    if (!en) {
        s_laser_present = false;
        s_relay_triggered = false;
        s_hold_completed = false;
        s_hold_total_ms = 0;
        s_hold_streak_ms = 0;
        s_last_heartbeat_ms = 0;
        laser_stop_audio();
        if (s_laser_timeout && esp_timer_is_active(s_laser_timeout)) {
            esp_timer_stop(s_laser_timeout);
        }
    }
}

void web_ui_laser_handle_heartbeat(const char *payload)
{
    (void)payload;
    laser_lock();
    if (!s_laser_cfg.enabled) {
        laser_unlock();
        return;
    }
    if (s_hold_completed) {
        laser_arm_timeout();
        laser_unlock();
        return;
    }
    int64_t now = now_ms();
    if (s_laser_present && s_last_heartbeat_ms > 0) {
        int64_t delta = now - s_last_heartbeat_ms;
        if (delta < 0) {
            delta = 0;
        }
        if (delta > 2000) {
            delta = 2000;
        }
        s_hold_total_ms += delta;
        s_hold_streak_ms += delta;
    } else {
        s_hold_streak_ms = 0;
    }
    s_laser_present = true;
    s_last_heartbeat_ms = now;
    laser_start_audio();
    laser_maybe_trigger_relay();
    laser_maybe_send_robot(now);
    laser_arm_timeout();
    laser_unlock();
}

static esp_err_t laser_config_handler(httpd_req_t *req)
{
    char resp[320];
    laser_cfg_t snapshot = {0};
    laser_lock();
    snapshot = s_laser_cfg;
    laser_unlock();
    int w = snprintf(resp, sizeof(resp),
                     "{\"hold\":\"%s\",\"relay\":\"%s\",\"seconds\":%d,\"mode\":\"%s\",\"enabled\":%s}",
                     snapshot.track_hold, snapshot.track_relay, snapshot.hold_seconds,
                     snapshot.cumulative ? "cumulative" : "continuous",
                     snapshot.enabled ? "true" : "false");
    if (w < 0 || w >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "resp too long");
    }
    return web_ui_send_ok(req, "application/json", resp);
}

static esp_err_t laser_save_handler(httpd_req_t *req)
{
    char q[384];
    char hold_enc[96] = {0}, relay_enc[96] = {0}, mode[16] = {0}, sec[8] = {0}, en[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "hold", hold_enc, sizeof(hold_enc));
        httpd_query_key_value(q, "relay", relay_enc, sizeof(relay_enc));
        httpd_query_key_value(q, "mode", mode, sizeof(mode));
        httpd_query_key_value(q, "seconds", sec, sizeof(sec));
        httpd_query_key_value(q, "enabled", en, sizeof(en));
    }
    char hold[96] = {0}, relay[96] = {0};
    web_ui_url_decode(hold, sizeof(hold), hold_enc);
    web_ui_url_decode(relay, sizeof(relay), relay_enc);
    bool en_param = false;
    bool en_val = false;
    if (en[0]) {
        en_param = true;
        en_val = (strcasecmp(en, "true") == 0) || (strcmp(en, "1") == 0) || (strcasecmp(en, "on") == 0);
    }
    laser_lock();
    if (hold[0]) {
        strncpy(s_laser_cfg.track_hold, hold, sizeof(s_laser_cfg.track_hold) - 1);
    }
    if (relay[0]) {
        strncpy(s_laser_cfg.track_relay, relay, sizeof(s_laser_cfg.track_relay) - 1);
    }
    if (sec[0]) {
        int v = atoi(sec);
        if (v > 0 && v < 3600) {
            s_laser_cfg.hold_seconds = v;
        }
    }
    if (mode[0]) {
        s_laser_cfg.cumulative = (strcasecmp(mode, "cumulative") == 0);
    }
    if (en_param) {
        laser_set_enabled_locked(en_val);
    }
    s_relay_triggered = false;
    s_hold_total_ms = 0;
    s_hold_streak_ms = 0;
    esp_err_t err = laser_save(&s_laser_cfg);
    laser_cfg_t snapshot = s_laser_cfg;
    laser_unlock();
    ESP_LOGI(TAG, "laser saved hold='%s' relay='%s' sec=%d mode=%s en=%s err=%s",
             snapshot.track_hold, snapshot.track_relay, snapshot.hold_seconds,
             snapshot.cumulative ? "cumulative" : "continuous",
             snapshot.enabled ? "on" : "off", esp_err_to_name(err));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
    }
    device_manager_update_laser_relay_track(s_laser_cfg.track_relay);
    return web_ui_send_ok(req, "text/plain", "laser saved");
}

static esp_err_t laser_enable_handler(httpd_req_t *req)
{
    char q[64];
    char state[8] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "state", state, sizeof(state));
    }
    bool en = (strcasecmp(state, "on") == 0) || (strcmp(state, "1") == 0) || (strcasecmp(state, "true") == 0);
    laser_lock();
    laser_set_enabled_locked(en);
    laser_save(&s_laser_cfg);
    bool current = s_laser_cfg.enabled;
    laser_unlock();
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"enabled\":%s}", current ? "true" : "false");
    return web_ui_send_ok(req, "application/json", resp);
}

esp_err_t web_ui_laser_register(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_uri_t laser_cfg = {.uri = "/api/laser/config", .method = HTTP_GET, .handler = laser_config_handler};
    httpd_uri_t laser_save_uri = {.uri = "/api/laser/save", .method = HTTP_GET, .handler = laser_save_handler};
    httpd_uri_t laser_enable = {.uri = "/api/laser/enable", .method = HTTP_GET, .handler = laser_enable_handler};
    esp_err_t err = httpd_register_uri_handler(server, &laser_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &laser_save_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &laser_enable);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

esp_err_t web_ui_laser_init(void)
{
    laser_load(&s_laser_cfg);
    device_manager_update_laser_relay_track(s_laser_cfg.track_relay);
    if (!s_laser_mutex) {
        s_laser_mutex = xSemaphoreCreateMutex();
    }
    ESP_RETURN_ON_FALSE(s_laser_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex create failed");
    s_robot_scanned = false;
    s_robot_track_count = 0;
    s_robot_last_ms = 0;
    return ESP_OK;
}
