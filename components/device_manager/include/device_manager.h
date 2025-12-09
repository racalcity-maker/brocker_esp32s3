#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_MANAGER_MAX_DEVICES               12
#define DEVICE_MANAGER_MAX_TABS                  12
#define DEVICE_MANAGER_MAX_TOPICS_PER_DEVICE     6
#define DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE  8
#define DEVICE_MANAGER_MAX_STEPS_PER_SCENARIO   16
#define DEVICE_MANAGER_MAX_FLAG_RULES            4
#define DEVICE_MANAGER_MAX_PROFILES              4
#define DEVICE_MANAGER_ID_MAX_LEN               32
#define DEVICE_MANAGER_NAME_MAX_LEN             48
#define DEVICE_MANAGER_TOPIC_MAX_LEN            96
#define DEVICE_MANAGER_PAYLOAD_MAX_LEN         160
#define DEVICE_MANAGER_FLAG_NAME_MAX_LEN        32
#define DEVICE_MANAGER_TRACK_NAME_MAX_LEN       64

#define DEVICE_MANAGER_DEVICE_ID_PICTURES   "pictures_core"
#define DEVICE_MANAGER_DEVICE_ID_LASER      "laser_core"
#define DEVICE_MANAGER_SCENARIO_PICTURES_OK   "pictures_ok"
#define DEVICE_MANAGER_SCENARIO_PICTURES_FAIL "pictures_fail"
#define DEVICE_MANAGER_SCENARIO_LASER_TRIGGER "laser_trigger"

typedef enum {
    DEVICE_TAB_AUDIO = 0,
    DEVICE_TAB_PICTURES,
    DEVICE_TAB_LASER,
    DEVICE_TAB_ROBOT,
    DEVICE_TAB_CUSTOM,
} device_tab_type_t;

typedef struct {
    device_tab_type_t type;
    char label[DEVICE_MANAGER_NAME_MAX_LEN];
    char extra_payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
} device_tab_t;

typedef struct {
    char name[DEVICE_MANAGER_NAME_MAX_LEN];
    char topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
} device_topic_binding_t;

typedef enum {
    DEVICE_ACTION_NOP = 0,
    DEVICE_ACTION_MQTT_PUBLISH,
    DEVICE_ACTION_AUDIO_PLAY,
    DEVICE_ACTION_AUDIO_STOP,
    DEVICE_ACTION_LASER_TRIGGER,
    DEVICE_ACTION_SET_FLAG,
    DEVICE_ACTION_WAIT_FLAGS,
    DEVICE_ACTION_LOOP,
    DEVICE_ACTION_DELAY,
    DEVICE_ACTION_EVENT_BUS,
} device_action_type_t;

typedef enum {
    DEVICE_CONDITION_ALL = 0,
    DEVICE_CONDITION_ANY,
} device_condition_type_t;

typedef struct {
    char flag[DEVICE_MANAGER_FLAG_NAME_MAX_LEN];
    bool required_state;
} device_flag_requirement_t;

typedef struct {
    device_condition_type_t mode;
    uint8_t requirement_count;
    device_flag_requirement_t requirements[DEVICE_MANAGER_MAX_FLAG_RULES];
    uint32_t timeout_ms;
} device_wait_flags_t;

typedef struct {
    char topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
    uint8_t qos;
    bool retain;
} device_mqtt_publish_t;

typedef struct {
    char track[DEVICE_MANAGER_TRACK_NAME_MAX_LEN];
    bool blocking;
} device_audio_action_t;

typedef struct {
    char event[DEVICE_MANAGER_NAME_MAX_LEN];
    char topic[DEVICE_MANAGER_TOPIC_MAX_LEN];
    char payload[DEVICE_MANAGER_PAYLOAD_MAX_LEN];
} device_event_action_t;

typedef struct {
    union {
        struct {
            uint16_t target_step;
            uint16_t max_iterations;
        } loop;
        device_wait_flags_t wait_flags;
        device_mqtt_publish_t mqtt;
        device_audio_action_t audio;
        struct {
            char flag[DEVICE_MANAGER_FLAG_NAME_MAX_LEN];
            bool value;
        } flag;
        device_event_action_t event;
    } data;
    uint32_t delay_ms;
    device_action_type_t type;
} device_action_step_t;

typedef struct {
    char id[DEVICE_MANAGER_ID_MAX_LEN];
    char name[DEVICE_MANAGER_NAME_MAX_LEN];
    uint8_t step_count;
    device_action_step_t steps[DEVICE_MANAGER_MAX_STEPS_PER_SCENARIO];
} device_scenario_t;

typedef struct {
    char id[DEVICE_MANAGER_ID_MAX_LEN];
    char display_name[DEVICE_MANAGER_NAME_MAX_LEN];
    uint8_t tab_count;
    device_tab_t tabs[DEVICE_MANAGER_MAX_TABS];
    uint8_t topic_count;
    device_topic_binding_t topics[DEVICE_MANAGER_MAX_TOPICS_PER_DEVICE];
    uint8_t scenario_count;
    device_scenario_t scenarios[DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE];
} device_descriptor_t;

typedef struct {
    char id[DEVICE_MANAGER_ID_MAX_LEN];
    char name[DEVICE_MANAGER_NAME_MAX_LEN];
    uint8_t device_count;
    device_descriptor_t devices[DEVICE_MANAGER_MAX_DEVICES];
} device_manager_profile_t;

typedef struct {
    uint32_t schema_version;
    uint32_t generation;
    uint8_t device_count;
    uint8_t tab_limit;
    uint8_t profile_count;
    char active_profile[DEVICE_MANAGER_ID_MAX_LEN];
    device_manager_profile_t profiles[DEVICE_MANAGER_MAX_PROFILES];
    device_descriptor_t devices[DEVICE_MANAGER_MAX_DEVICES];
} device_manager_config_t;

esp_err_t device_manager_init(void);
esp_err_t device_manager_reload_from_nvs(void);
esp_err_t device_manager_save_snapshot(void);
const device_manager_config_t *device_manager_get(void);
esp_err_t device_manager_apply(const device_manager_config_t *next);
esp_err_t device_manager_sync_file(void);
esp_err_t device_manager_export_json(char **out_json, size_t *out_len);
esp_err_t device_manager_export_profile_json(const char *profile_id, char **out_json, size_t *out_len);
esp_err_t device_manager_apply_json(const char *json, size_t len);
esp_err_t device_manager_apply_profile_json(const char *profile_id, const char *json, size_t len);
esp_err_t device_manager_update_pictures_tracks(const char *ok_track, const char *fail_track);
esp_err_t device_manager_update_laser_relay_track(const char *relay_track);
esp_err_t device_manager_profile_create(const char *id, const char *name, const char *clone_id);
esp_err_t device_manager_profile_delete(const char *id);
esp_err_t device_manager_profile_rename(const char *id, const char *new_name);
esp_err_t device_manager_profile_activate(const char *id);

#ifdef __cplusplus
}
#endif
