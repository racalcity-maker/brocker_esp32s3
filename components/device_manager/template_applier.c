#include "dm_template_applier.h"

#include <string.h>

#include "device_manager_utils.h"
#include "dm_template_runtime.h"

static device_descriptor_t *find_device(device_manager_config_t *cfg, const char *id)
{
    if (!cfg || !id || !id[0]) {
        return NULL;
    }
    for (uint8_t i = 0; i < cfg->device_count && i < DEVICE_MANAGER_MAX_DEVICES; ++i) {
        device_descriptor_t *dev = &cfg->devices[i];
        if (dev->id[0] && strcasecmp(dev->id, id) == 0) {
            return dev;
        }
    }
    return NULL;
}

static device_descriptor_t *ensure_device(device_manager_config_t *cfg, const char *id, const char *name)
{
    device_descriptor_t *dev = find_device(cfg, id);
    if (dev) {
        memset(dev, 0, sizeof(*dev));
    } else {
        if (cfg->device_count >= DEVICE_MANAGER_MAX_DEVICES) {
            return NULL;
        }
        dev = &cfg->devices[cfg->device_count++];
        memset(dev, 0, sizeof(*dev));
    }
    dm_str_copy(dev->id, sizeof(dev->id), id);
    dm_str_copy(dev->display_name, sizeof(dev->display_name), name ? name : id);
    return dev;
}

static device_action_step_t *scenario_add_step(device_scenario_t *scenario)
{
    if (!scenario) {
        return NULL;
    }
    if (scenario->step_count >= DEVICE_MANAGER_MAX_STEPS_PER_SCENARIO) {
        return NULL;
    }
    device_action_step_t *step = &scenario->steps[scenario->step_count++];
    memset(step, 0, sizeof(*step));
    return step;
}

static void add_mqtt_step(device_scenario_t *scenario, const char *topic, const char *payload)
{
    if (!topic || !topic[0]) {
        return;
    }
    device_action_step_t *step = scenario_add_step(scenario);
    if (!step) {
        return;
    }
    step->type = DEVICE_ACTION_MQTT_PUBLISH;
    dm_str_copy(step->data.mqtt.topic, sizeof(step->data.mqtt.topic), topic);
    if (payload) {
        dm_str_copy(step->data.mqtt.payload, sizeof(step->data.mqtt.payload), payload);
    }
    step->data.mqtt.qos = 0;
    step->data.mqtt.retain = false;
}

static void add_audio_step(device_scenario_t *scenario, const char *track, bool blocking)
{
    if (!track || !track[0]) {
        return;
    }
    device_action_step_t *step = scenario_add_step(scenario);
    if (!step) {
        return;
    }
    step->type = DEVICE_ACTION_AUDIO_PLAY;
    dm_str_copy(step->data.audio.track, sizeof(step->data.audio.track), track);
    step->data.audio.blocking = blocking;
}

static void add_delay_step(device_scenario_t *scenario, uint32_t delay_ms)
{
    if (delay_ms == 0) {
        return;
    }
    device_action_step_t *step = scenario_add_step(scenario);
    if (!step) {
        return;
    }
    step->type = DEVICE_ACTION_DELAY;
    step->delay_ms = delay_ms;
}

static void init_scenario(device_descriptor_t *dev,
                          device_scenario_t *scenario,
                          const char *id,
                          const char *name)
{
    memset(scenario, 0, sizeof(*scenario));
    dm_str_copy(scenario->id, sizeof(scenario->id), id);
    dm_str_copy(scenario->name, sizeof(scenario->name), name);
    scenario->step_count = 0;
}

static bool has_uid_success_actions(const dm_uid_template_t *tpl)
{
    return (tpl->success_topic[0] ||
            tpl->success_audio_track[0] ||
            tpl->success_signal_topic[0]);
}

static bool has_uid_fail_actions(const dm_uid_template_t *tpl)
{
    return (tpl->fail_topic[0] ||
            tpl->fail_audio_track[0] ||
            tpl->fail_signal_topic[0]);
}

static void build_uid_scenario(device_descriptor_t *dev,
                               const char *scenario_id,
                               const char *scenario_name,
                               const char *topic,
                               const char *payload,
                               const char *audio_track,
                               const char *signal_topic,
                               const char *signal_payload)
{
    if (!topic && !audio_track && !signal_topic) {
        return;
    }
    if (dev->scenario_count >= DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE) {
        return;
    }
    device_scenario_t *scenario = &dev->scenarios[dev->scenario_count++];
    init_scenario(dev, scenario, scenario_id, scenario_name);
    add_mqtt_step(scenario, topic, payload);
    add_audio_step(scenario, audio_track, false);
    add_mqtt_step(scenario, signal_topic, signal_payload);
}

static esp_err_t apply_uid_template(device_descriptor_t *dev, const dm_uid_template_t *tpl)
{
    dev->tab_count = 0;
    dev->topic_count = 0;
    dev->scenario_count = 0;

    if (has_uid_success_actions(tpl)) {
        build_uid_scenario(dev,
                           "uid_success",
                           "UID Success",
                           tpl->success_topic,
                           tpl->success_payload,
                           tpl->success_audio_track,
                           tpl->success_signal_topic,
                           tpl->success_signal_payload);
    }

    if (has_uid_fail_actions(tpl)) {
        build_uid_scenario(dev,
                           "uid_fail",
                           "UID Fail",
                           tpl->fail_topic,
                           tpl->fail_payload,
                           tpl->fail_audio_track,
                           tpl->fail_signal_topic,
                           tpl->fail_signal_payload);
    }

    return (dev->scenario_count > 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t apply_signal_template(device_descriptor_t *dev, const dm_signal_hold_template_t *tpl)
{
    dev->tab_count = 0;
    dev->topic_count = 0;
    dev->scenario_count = 0;

    if (dev->scenario_count >= DEVICE_MANAGER_MAX_SCENARIOS_PER_DEVICE) {
        return ESP_ERR_INVALID_STATE;
    }
    device_scenario_t *scenario = &dev->scenarios[dev->scenario_count++];
    init_scenario(dev, scenario, "signal_complete", "Signal Hold Complete");

    add_mqtt_step(scenario, tpl->signal_topic, tpl->signal_payload_on);
    add_audio_step(scenario, tpl->complete_track, false);
    if (tpl->signal_on_ms > 0) {
        add_delay_step(scenario, tpl->signal_on_ms);
    }
    add_mqtt_step(scenario, tpl->signal_topic, tpl->signal_payload_off);
    return ESP_OK;
}

esp_err_t dm_template_apply_to_config(device_manager_config_t *cfg,
                                      const dm_template_config_t *tpl,
                                      const char *device_id,
                                      const char *display_name)
{
    if (!cfg || !tpl || !device_id || !device_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    device_descriptor_t *dev = ensure_device(cfg, device_id, display_name ? display_name : device_id);
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t res = ESP_ERR_NOT_SUPPORTED;
    switch (tpl->type) {
    case DM_TEMPLATE_TYPE_UID:
        res = apply_uid_template(dev, &tpl->data.uid);
        break;
    case DM_TEMPLATE_TYPE_SIGNAL_HOLD:
        res = apply_signal_template(dev, &tpl->data.signal);
        break;
    default:
        res = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    if (res == ESP_OK) {
        dm_template_runtime_register(tpl, device_id);
    }
    return res;
}
